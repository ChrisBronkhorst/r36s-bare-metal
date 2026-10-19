/* gpuc: build a compute shader with the NIR builder, compile it for
 * Bifrost v7 (Mali-G31) with Mesa's backend, and write the machine code
 * plus what the descriptors need. For the R36S bare-metal kernel. */
#include <stdio.h>
#include <string.h>
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/pan_compiler.h"
#include "compiler/bifrost/bifrost_compile.h"
#include "model/pan_model.h"
#include "util/u_dynarray.h"

#define ARCH 7

/* out[i] = i*i + 7, with `out` (64-bit) in push constant bytes 0-7 */
static nir_shader *build_ids(const nir_shader_compiler_options *opt)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, opt, "ids");
   b.shader->info.workgroup_size[0] = 16;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   nir_def *id = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *out = nir_load_push_constant(&b, 1, 64, nir_imm_int(&b, 0), .base = 0, .range = 8);
   nir_def *addr = nir_iadd(&b, out, nir_u2u64(&b, nir_imul_imm(&b, id, 4)));
   nir_def *val = nir_iadd_imm(&b, nir_imul(&b, id, id), 7);
   nir_build_store_global(&b, val, addr, .align_mul = 4, .write_mask = 1);
   return b.shader;
}

/* Matrix multiply, C = A x B, n x n floats, row by row in memory. Push
 * constants: A, B, C (64-bit addresses) at bytes 0, 8, 16, n at 24. */
static nir_def *push64(nir_builder *b, unsigned at)
{
   /* The byte offset goes in the operand: this backend ignores .base */
   return nir_load_push_constant(b, 1, 64, nir_imm_int(b, at), .range = 8);
}

static nir_def *element(nir_builder *b, nir_def *base, nir_def *index)
{
   return nir_iadd(b, base, nir_u2u64(b, nir_imul_imm(b, index, 4)));
}

/* mm1: one element of C per thread: thread (x, y) computes C[y][x] */
static nir_shader *build_mm1(const nir_shader_compiler_options *opt)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, opt, "mm1");
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;
   b.shader->info.workgroup_size[2] = 1;
   nir_def *gid = nir_load_global_invocation_id(&b, 32);
   nir_def *x = nir_channel(&b, gid, 0), *y = nir_channel(&b, gid, 1);
   nir_def *A = push64(&b, 0), *B = push64(&b, 8), *C = push64(&b, 16);
   nir_def *n = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 24), .range = 4);

   nir_variable *kv = nir_local_variable_create(b.impl, glsl_uint_type(), "k");
   nir_variable *sv = nir_local_variable_create(b.impl, glsl_float_type(), "sum");
   nir_store_var(&b, kv, nir_imm_int(&b, 0), 1);
   nir_store_var(&b, sv, nir_imm_float(&b, 0), 1);
   nir_loop *loop = nir_push_loop(&b);
   {
      nir_def *k = nir_load_var(&b, kv);
      nir_break_if(&b, nir_uge(&b, k, n));
      nir_def *a = nir_build_load_global(&b, 1, 32, element(&b, A, nir_iadd(&b, nir_imul(&b, y, n), k)), .align_mul = 4);
      nir_def *bb = nir_build_load_global(&b, 1, 32, element(&b, B, nir_iadd(&b, nir_imul(&b, k, n), x)), .align_mul = 4);
      nir_store_var(&b, sv, nir_ffma(&b, a, bb, nir_load_var(&b, sv)), 1);
      nir_store_var(&b, kv, nir_iadd_imm(&b, k, 1), 1);
   }
   nir_pop_loop(&b, loop);
   nir_build_store_global(&b, nir_load_var(&b, sv), element(&b, C, nir_iadd(&b, nir_imul(&b, y, n), x)),
                          .align_mul = 4, .write_mask = 1);
   return b.shader;
}

/* mm4x4: thread (x, y) computes the 4x4 block of C at row 4y, column 4x.
 * Per step of 4 in k: four vec4 loads of A (rows 4y..4y+3, k..k+3), four
 * of B (rows k..k+3, columns 4x..4x+3), then 64 fused multiply-adds, in
 * the same order as the CPU's NEON version (k ascending, one FMA each). */
static nir_shader *build_mm4x4(const nir_shader_compiler_options *opt)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, opt, "mm4x4");
   b.shader->info.workgroup_size[0] = 4;
   b.shader->info.workgroup_size[1] = 4;
   b.shader->info.workgroup_size[2] = 1;
   nir_def *gid = nir_load_global_invocation_id(&b, 32);
   nir_def *col = nir_imul_imm(&b, nir_channel(&b, gid, 0), 4);
   nir_def *row = nir_imul_imm(&b, nir_channel(&b, gid, 1), 4);
   nir_def *A = push64(&b, 0), *B = push64(&b, 8), *C = push64(&b, 16);
   nir_def *n = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 24), .range = 4);

   nir_variable *kv = nir_local_variable_create(b.impl, glsl_uint_type(), "k");
   nir_variable *acc[4];
   nir_store_var(&b, kv, nir_imm_int(&b, 0), 1);
   for (int r = 0; r < 4; r++) {
      acc[r] = nir_local_variable_create(b.impl, glsl_vec4_type(), "acc");
      nir_store_var(&b, acc[r], nir_imm_vec4(&b, 0, 0, 0, 0), 0xf);
   }
   nir_loop *loop = nir_push_loop(&b);
   {
      nir_def *k = nir_load_var(&b, kv);
      nir_break_if(&b, nir_uge(&b, k, n));
      nir_def *a[4], *bq[4];
      for (int r = 0; r < 4; r++)
         a[r] = nir_build_load_global(&b, 4, 32,
            element(&b, A, nir_iadd(&b, nir_imul(&b, nir_iadd_imm(&b, row, r), n), k)), .align_mul = 16);
      for (int q = 0; q < 4; q++)
         bq[q] = nir_build_load_global(&b, 4, 32,
            element(&b, B, nir_iadd(&b, nir_imul(&b, nir_iadd_imm(&b, k, q), n), col)), .align_mul = 16);
      for (int r = 0; r < 4; r++) {
         nir_def *sum = nir_load_var(&b, acc[r]);
         for (int q = 0; q < 4; q++)
            sum = nir_ffma(&b, nir_swizzle(&b, a[r], (unsigned[]){q, q, q, q}, 4), bq[q], sum);
         nir_store_var(&b, acc[r], sum, 0xf);
      }
      nir_store_var(&b, kv, nir_iadd_imm(&b, k, 4), 1);
   }
   nir_pop_loop(&b, loop);
   for (int r = 0; r < 4; r++)
      nir_build_store_global(&b, nir_load_var(&b, acc[r]),
         element(&b, C, nir_iadd(&b, nir_imul(&b, nir_iadd_imm(&b, row, r), n), col)),
         .align_mul = 16, .write_mask = 0xf);
   return b.shader;
}

int main(int argc, char **argv)
{
   if (argc < 3) {
      fprintf(stderr, "usage: gpuc SHADER OUT.bin\n");
      return 1;
   }
   glsl_type_singleton_init_or_ref();
   const nir_shader_compiler_options *opt =
      pan_get_nir_shader_compiler_options(ARCH, MESA_SHADER_COMPUTE, false);
   nir_shader *s;
   if (!strcmp(argv[1], "ids"))
      s = build_ids(opt);
   else if (!strcmp(argv[1], "mm1"))
      s = build_mm1(opt);
   else if (!strcmp(argv[1], "mm4x4"))
      s = build_mm4x4(opt);
   else {
      fprintf(stderr, "unknown shader %s\n", argv[1]);
      return 1;
   }

   struct pan_compile_inputs inputs = {
      .arch = ARCH,
      .model = pan_get_model(0x70930000, 0, 1, 0),
      .fau.reserved = strncmp(argv[1], "mm", 2) ? 2 : 8,   /* our push constants, in 32-bit words */
   };
   nir_lower_compute_system_values_options cs = {.global_id_is_32bit = true};
   NIR_PASS(_, s, nir_lower_vars_to_ssa);
   NIR_PASS(_, s, nir_lower_system_values);
   NIR_PASS(_, s, nir_lower_compute_system_values, &cs);
   pan_preprocess_nir(s, ARCH);
   struct pan_shader_info info = {0};
   pan_postprocess_nir(s, &inputs, &info);
   if (getenv("NIR"))
      nir_print_shader(s, stderr);

   struct util_dynarray bin = UTIL_DYNARRAY_INIT;
   pan_shader_compile(s, &inputs, &bin, &info);

   FILE *f = fopen(argv[2], "wb");
   fwrite(bin.data, 1, bin.size, f);
   fclose(f);
   printf("model %s; %u bytes; work registers %u; preload %016llx; "
          "fau count %u; tls %u; wls %u; ubo %u\n",
          inputs.model ? inputs.model->name : "(none)", (unsigned)bin.size,
          info.work_reg_count, (unsigned long long)info.preload, info.fau.count,
          info.tls_size, info.wls_size, info.ubo_count);
   return 0;
}
