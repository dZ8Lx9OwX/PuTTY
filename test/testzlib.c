/*
 * Main program to compile ssh/zlib.c into a zlib decoding tool.
 *
 * This is potentially a handy tool in its own right for picking apart
 * Zip files or PDFs or PNGs, because it accepts the bare Deflate
 * format and the zlib wrapper format, unlike 'zcat' which accepts
 * only the gzip wrapper format.
 *
 * It's also useful as a means for a fuzzer to get reasonably direct
 * access to PuTTY's zlib decompressor.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "defs.h"
#include "ssh.h"

void out_of_memory(void)
{
    fprintf(stderr, "内存不足!\n");
    exit(1);
}

void dputs(const char *buf)
{
    fputs(buf, stderr);
}

int main(int argc, char **argv)
{
    unsigned char buf[16], *outbuf;
    int ret, outlen;
    ssh_decompressor *handle;
    int noheader = false, opts = true;
    char *filename = NULL;
    FILE *fp;

    while (--argc) {
        char *p = *++argv;

        if (p[0] == '-' && opts) {
            if (!strcmp(p, "-d")) {
                noheader = true;
            } else if (!strcmp(p, "--")) {
                opts = false;          /* next thing is filename */
            } else if (!strcmp(p, "--help")) {
                printf("用法： testzlib          从标准输入解码zlib(RFC1950)"
                       "数据\n");
                printf("       testzlib -d       从标准输入解码(RFC1951)"
                       "数据\n");
                printf("       testzlib --help   显示帮助信息\n");
                return 0;
            } else {
                fprintf(stderr, "未知命令行选项 '%s'\n", p);
                return 1;
            }
        } else if (!filename) {
            filename = p;
        } else {
            fprintf(stderr, "只能处理一个文件名\n");
            return 1;
        }
    }

    handle = ssh_decompressor_new(&ssh_zlib);

    if (noheader) {
        /*
         * Provide missing zlib header if -d was specified.
         */
        static const unsigned char ersatz_zlib_header[] = { 0x78, 0x9C };
        ssh_decompressor_decompress(
            handle, ersatz_zlib_header, sizeof(ersatz_zlib_header),
            &outbuf, &outlen);
        assert(outlen == 0);
    }

    if (filename)
        fp = fopen(filename, "rb");
    else
        fp = stdin;

    if (!fp) {
        assert(filename);
        fprintf(stderr, "无法打开 '%s'\n", filename);
        return 1;
    }

    while (1) {
        ret = fread(buf, 1, sizeof(buf), fp);
        if (ret <= 0)
            break;
        ssh_decompressor_decompress(handle, buf, ret, &outbuf, &outlen);
        if (outbuf) {
            if (outlen)
                fwrite(outbuf, 1, outlen, stdout);
            sfree(outbuf);
        } else {
            fprintf(stderr, "解码错误\n");
            fclose(fp);
            return 1;
        }
    }

    ssh_decompressor_free(handle);

    if (filename)
        fclose(fp);

    return 0;
}
