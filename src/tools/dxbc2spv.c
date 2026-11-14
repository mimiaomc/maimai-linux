#include <stdio.h>
#include <stdlib.h>
#include <vkd3d_shader.h>

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize < 0) {
        fclose(f);
        return 2;
    }
    size_t size = (size_t)fsize;
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size ? size : 1);
    if (!buf) {
        fclose(f);
        return 3;
    }
    if (size > 0 && fread(buf, 1, size, f) != size) {
        fclose(f);
        free(buf);
        return 4;
    }
    fclose(f);

    struct vkd3d_shader_compile_info info = {0};
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.source.code = buf;
    info.source.size = size;
    info.source_type = VKD3D_SHADER_SOURCE_DXBC_TPF;
    info.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY;

    struct vkd3d_shader_code out_code = {0};
    char *messages = NULL;
    int ret = vkd3d_shader_compile(&info, &out_code, &messages);
    if (ret == 0) {
        FILE *out_f = fopen(argv[2], "wb");
        if (out_f) {
            fwrite(out_code.code, 1, out_code.size, out_f);
            fclose(out_f);
        }
        printf("Success! SPIR-V size: %zu\n", out_code.size);
        vkd3d_shader_free_shader_code(&out_code);
    } else {
        printf("Failed: %d, %s\n", ret, messages ? messages : "none");
    }
    if (messages) vkd3d_shader_free_messages(messages);
    free(buf);
    return ret;
}
