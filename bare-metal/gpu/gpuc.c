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
   else {
      fprintf(stderr, "unknown shader %s\n", argv[1]);
      return 1;
   }

   struct pan_compile_inputs inputs = {
      .arch = ARCH,
      .model = pan_get_model(0x70930000, 0, 1, 0),
      .fau.reserved = 2,   /* our 2 words of push constants */
   };
   nir_lower_compute_system_values_options cs = {.global_id_is_32bit = true};
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
