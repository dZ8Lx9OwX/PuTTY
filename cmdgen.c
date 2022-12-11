/*
 * cmdgen.c - command-line form of PuTTYgen
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <assert.h>
#include <time.h>
#include <errno.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "sshkeygen.h"
#include "mpint.h"

static FILE *progress_fp = NULL;
static bool linear_progress_phase;
static unsigned last_progress_col;

static ProgressPhase cmdgen_progress_add_linear(
    ProgressReceiver *prog, double c)
{
    ProgressPhase ph = { .n = 0 };
    return ph;
}

static ProgressPhase cmdgen_progress_add_probabilistic(
    ProgressReceiver *prog, double c, double p)
{
    ProgressPhase ph = { .n = 1 };
    return ph;
}

static void cmdgen_progress_start_phase(ProgressReceiver *prog,
                                        ProgressPhase p)
{
    linear_progress_phase = (p.n == 0);
    last_progress_col = 0;
}
static void cmdgen_progress_report(ProgressReceiver *prog, double p)
{
    unsigned new_col = p * 64 + 0.5;
    for (; last_progress_col < new_col; last_progress_col++)
        fputc('+', progress_fp);
}
static void cmdgen_progress_report_attempt(ProgressReceiver *prog)
{
    if (progress_fp) {
        fputc('+', progress_fp);
        fflush(progress_fp);
    }
}
static void cmdgen_progress_report_phase_complete(ProgressReceiver *prog)
{
    if (linear_progress_phase)
        cmdgen_progress_report(prog, 1.0);
    if (progress_fp) {
        fputc('\n', progress_fp);
        fflush(progress_fp);
    }
}

static const ProgressReceiverVtable cmdgen_progress_vt = {
    .add_linear = cmdgen_progress_add_linear,
    .add_probabilistic = cmdgen_progress_add_probabilistic,
    .ready = null_progress_ready,
    .start_phase = cmdgen_progress_start_phase,
    .report = cmdgen_progress_report,
    .report_attempt = cmdgen_progress_report_attempt,
    .report_phase_complete = cmdgen_progress_report_phase_complete,
};

static ProgressReceiver cmdgen_progress = { .vt = &cmdgen_progress_vt };

/*
 * Stubs to let everything else link sensibly.
 */
char *x_get_default(const char *key)
{
    return NULL;
}
void sk_cleanup(void)
{
}

void showversion(void)
{
    char *buildinfo_text = buildinfo("\n");
    printf("puttygen: %s\n%s\n", ver, buildinfo_text);
    sfree(buildinfo_text);
}

void usage(bool standalone)
{
    fprintf(standalone ? stderr : stdout,
            "用法: puttygen ( 密钥文件 | -t 类型 [ -b 位数 ] )\n"
            "                [ -C 注释 ] [ -P ] [ -q ]\n"
            "                [ -o 输出密钥文件 ] [ -O 类型 | -l | -L"
            " | -p ]\n");
    if (standalone)
        fprintf(stderr,
                "使用 \"puttygen --help\" 了解更多详情.\n");
}

void help(void)
{
    /*
     * Help message is an extended version of the usage message. So
     * start with that, plus a version heading.
     */
    printf("PuTTYgen: PuTTY的密钥生成和转换工具\n"
           "%s\n", ver);
    usage(false);
    printf("  -t    生成时指定密钥类型:\n"
           "           eddsa, ecdsa, rsa, dsa, rsa1   用于 -b 参数\n"
           "           ed25519, ed448                 用于eddsa的特殊情况\n"
           "  -b    生成密钥时指定位数\n"
           "  -C    更改或者指定密钥的注释信息\n"
           "  -P    更改密钥密码\n"
           "  -q    不显示进度条\n"
           "  -O    指定输出类型:\n"
           "           private             输出PuTTY私钥格式\n"
           "           private-openssh     导出OpenSSH私钥\n"
           "           private-openssh-new 导出OpenSSH私钥 "
                                             "(强制新格式)\n"
           "           private-sshcom      导出ssh.com私钥\n"
           "           public              RFC 4716/ssh.com公钥\n"
           "           public-openssh      OpenSSH公钥\n"
           "           fingerprint         输出密钥指纹\n"
           "           cert-info           输出证书信息\n"
           "           text                输出密钥组件信息，如 "
           "'name=0x####'\n"
           "  -o    指定输出文件\n"
           "  -l    相当于 `-O fingerprint'\n"
           "  -L    相当于 `-O public-openssh'\n"
           "  -p    相当于 `-O public'\n"
           "  --cert-info   相当于 `-O cert-info'\n"
           "  --dump   相当于 `-O text'\n"
           "  -E fptype            指定指纹输出类型：\n"
           "                          sha256, md5, sha256-cert, md5-cert\n"
           "  --certificate file   将证书添加到密钥\n"
           "  --remove-certificate 从密钥中删除任何证书\n"
           "  --reencrypt          加载密钥并使用新格式"
           "保存\n"
           "  --old-passphrase 文件\n"
           "        指定包含旧密钥密码的文件\n"
           "  --new-passphrase 文件\n"
           "        指定包含新密钥密码的文件\n"
           "  --random-device 设备\n"
           "        指定从设备中读取 (例如 /dev/urandom)\n"
           "  --primes <type>      选择素数生成的方法:\n"
           "        probable       使用常规概率的素数(快速)\n"
           "        proven         使用经过验证的素数(较慢)\n"
           "        proven-even    使用经过验证且均匀分布的素数(最慢)\n"
           "  --strong-rsa         使用\"强\"素数作为RSA密钥要素\n"
           "  --ppk-param <key>=<value>[,<key>=<value>,...]\n"
           "            写入PuTTY私钥文件时指定参数格式:\n"
           "                version       PPK格式版本 (最小 2,最大 3,默认 3)\n"
           "                kdf           密钥导出函数 (argon2id, argon2i, argon2d)\n"
           "                memory        用于密钥哈希的内存Kbyte (默认 8192)\n"
           "                time          密钥哈希的毫秒数 (默认 100)\n"
           "                passes        要运行的哈希传递数 (替换'time')\n"
           "                parallelism   哈希函数中的并行线程数 (默认 1)\n"
           );
}

static bool move(char *from, char *to)
{
    int ret;

    ret = rename(from, to);
    if (ret) {
        /*
         * This OS may require us to remove the original file first.
         */
        remove(to);
        ret = rename(from, to);
    }
    if (ret) {
        perror("puttygen: 无法将新文件覆盖旧文件");
        return false;
    }
    return true;
}

static char *readpassphrase(const char *filename)
{
    FILE *fp;
    char *line;

    fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "puttygen: 无法打开 %s: %s\n",
                filename, strerror(errno));
        return NULL;
    }
    line = fgetline(fp);
    if (line)
        line[strcspn(line, "\r\n")] = '\0';
    else if (ferror(fp))
        fprintf(stderr, "puttygen: 读取错误 %s: %s\n",
                filename, strerror(errno));
    else        /* empty file */
        line = dupstr("");
    fclose(fp);
    return line;
}

#define DEFAULT_RSADSA_BITS 2048

static void spr_error(SeatPromptResult spr)
{
    if (spr.kind == SPRK_SW_ABORT) {
        char *err = spr_get_error_message(spr);
        fprintf(stderr, "puttygen: 无法读取密码: %s", err);
        sfree(err);
    }
}

/* For Unix in particular, but harmless if this main() is reused elsewhere */
const bool buildinfo_gtk_relevant = false;

int main(int argc, char **argv)
{
    char *infile = NULL;
    Filename *infilename = NULL, *outfilename = NULL;
    LoadedFile *infile_lf = NULL;
    BinarySource *infile_bs = NULL;
    enum { NOKEYGEN, RSA1, RSA2, DSA, ECDSA, EDDSA } keytype = NOKEYGEN;
    char *outfile = NULL, *outfiletmp = NULL;
    enum { PRIVATE, PUBLIC, PUBLICO, FP, OPENSSH_AUTO,
           OPENSSH_NEW, SSHCOM, TEXT, CERTINFO } outtype = PRIVATE;
    int bits = -1;
    const char *comment = NULL;
    char *origcomment = NULL;
    bool change_passphrase = false, reencrypt = false;
    bool errs = false, nogo = false;
    int intype = SSH_KEYTYPE_UNOPENABLE;
    int sshver = 0;
    ssh2_userkey *ssh2key = NULL;
    RSAKey *ssh1key = NULL;
    strbuf *ssh2blob = NULL;
    char *ssh2alg = NULL;
    char *old_passphrase = NULL, *new_passphrase = NULL;
    bool load_encrypted;
    const char *random_device = NULL;
    char *certfile = NULL;
    bool remove_cert = false;
    int exit_status = 0;
    const PrimeGenerationPolicy *primegen = &primegen_probabilistic;
    bool strong_rsa = false;
    ppk_save_parameters params = ppk_save_default_parameters;
    FingerprintType fptype = SSH_FPTYPE_DEFAULT;

    if (is_interactive())
        progress_fp = stderr;

    #define RETURN(status) do { exit_status = (status); goto out; } while (0)

    /* ------------------------------------------------------------------
     * Parse the command line to figure out what we've been asked to do.
     */

    /*
     * If run with no arguments at all, print the usage message and
     * return success.
     */
    if (argc <= 1) {
        usage(true);
        RETURN(0);
    }

    /*
     * Parse command line arguments.
     */
    while (--argc) {
        char *p = *++argv;
        if (p[0] == '-' && p[1]) {
            /*
             * An option.
             */
            while (p && *++p) {
                char c = *p;
                switch (c) {
                  case '-': {
                    /*
                     * Long option.
                     */
                    char *opt, *val;
                    opt = p++;     /* opt will have _one_ leading - */
                    while (*p && *p != '=')
                        p++;               /* find end of option */
                    if (*p == '=') {
                        *p++ = '\0';
                        val = p;
                    } else
                        val = NULL;

                    if (!strcmp(opt, "-help")) {
                        if (val) {
                            errs = true;
                            fprintf(stderr, "puttygen: 选项 `-%s'"
                                    " 预期无参数\n", opt);
                        } else {
                            help();
                            nogo = true;
                        }
                    } else if (!strcmp(opt, "-version")) {
                        if (val) {
                            errs = true;
                            fprintf(stderr, "puttygen: 选项 `-%s'"
                                    " 预期无参数\n", opt);
                        } else {
                            showversion();
                            nogo = true;
                        }
                    } else if (!strcmp(opt, "-pgpfp")) {
                        if (val) {
                            errs = true;
                            fprintf(stderr, "puttygen: 选项 `-%s'"
                                    " 预期无参数\n", opt);
                        } else {
                            /* support --pgpfp for consistency */
                            pgp_fingerprints();
                            nogo = true;
                        }
                    } else if (!strcmp(opt, "-old-passphrase")) {
                        if (!val && argc > 1)
                            --argc, val = *++argv;
                        if (!val) {
                            errs = true;
                            fprintf(stderr, "puttygen: 选项 `-%s'"
                                    " 需要一个参数\n", opt);
                        } else {
                            old_passphrase = readpassphrase(val);
                            if (!old_passphrase)
                                errs = true;
                        }
                    } else if (!strcmp(opt, "-new-passphrase")) {
                        if (!val && argc > 1)
                            --argc, val = *++argv;
                        if (!val) {
                            errs = true;
                            fprintf(stderr, "puttygen: 选项 `-%s'"
                                    " 需要一个参数\n", opt);
                        } else {
                            new_passphrase = readpassphrase(val);
                            if (!new_passphrase)
                                errs = true;
                        }
                    } else if (!strcmp(opt, "-random-device")) {
                        if (!val && argc > 1)
                            --argc, val = *++argv;
                        if (!val) {
                            errs = true;
                            fprintf(stderr, "puttygen: 选项 `-%s'"
                                    " 预期无参数\n", opt);
                        } else {
                            random_device = val;
                        }
                    } else if (!strcmp(opt, "-dump")) {
                        outtype = TEXT;
                    } else if (!strcmp(opt, "-cert-info") ||
                               !strcmp(opt, "-certinfo") ||
                               !strcmp(opt, "-cert_info")) {
                        outtype = CERTINFO;
                    } else if (!strcmp(opt, "-primes")) {
                        if (!val && argc > 1)
                            --argc, val = *++argv;
                        if (!val) {
                            errs = true;
                            fprintf(stderr, "puttygen: 选项 `-%s'"
                                    " 需要一个参数\n", opt);
                        } else if (!strcmp(val, "probable") ||
                                   !strcmp(val, "probabilistic")) {
                            primegen = &primegen_probabilistic;
                        } else if (!strcmp(val, "provable") ||
                                   !strcmp(val, "proven") ||
                                   !strcmp(val, "simple") ||
                                   !strcmp(val, "maurer-simple")) {
                            primegen = &primegen_provable_maurer_simple;
                        } else if (!strcmp(val, "provable-even") ||
                                   !strcmp(val, "proven-even") ||
                                   !strcmp(val, "even") ||
                                   !strcmp(val, "complex") ||
                                   !strcmp(val, "maurer-complex")) {
                            primegen = &primegen_provable_maurer_complex;
                        } else {
                            errs = true;
                            fprintf(stderr, "puttygen: 无法识别素数"
                                    "生成模式 `%s'\n", val);
                        }
                    } else if (!strcmp(opt, "-strong-rsa")) {
                        strong_rsa = true;
                    } else if (!strcmp(opt, "-certificate")) {
                        if (!val && argc > 1)
                            --argc, val = *++argv;
                        if (!val) {
                            errs = true;
                            fprintf(stderr, "puttygen: 选项 `-%s'"
                                    " 需要一个参数\n", opt);
                        } else {
                            certfile = val;
                        }
                    } else if (!strcmp(opt, "-remove-certificate")) {
                        remove_cert = true;
                    } else if (!strcmp(opt, "-reencrypt")) {
                        reencrypt = true;
                    } else if (!strcmp(opt, "-ppk-param") ||
                               !strcmp(opt, "-ppk-params")) {
                        if (!val && argc > 1)
                            --argc, val = *++argv;
                        if (!val) {
                            errs = true;
                            fprintf(stderr, "puttygen: 选项 `-%s'"
                                    " 需要一个参数\n", opt);
                        } else {
                            char *nextval;
                            for (; val; val = nextval) {
                                nextval = strchr(val, ',');
                                if (nextval)
                                    *nextval++ = '\0';

                                char *optvalue = strchr(val, '=');
                                if (!optvalue) {
                                    errs = true;
                                    fprintf(stderr, "puttygen: PPK参数 "
                                            "'%s' 需要一个值\n", val);
                                    continue;
                                }
                                *optvalue++ = '\0';

                                /* Non-numeric options */
                                if (!strcmp(val, "kdf")) {
                                    if (!strcmp(optvalue, "Argon2id") ||
                                        !strcmp(optvalue, "argon2id")) {
                                        params.argon2_flavour = Argon2id;
                                    } else if (!strcmp(optvalue, "Argon2i") ||
                                               !strcmp(optvalue, "argon2i")) {
                                        params.argon2_flavour = Argon2i;
                                    } else if (!strcmp(optvalue, "Argon2d") ||
                                               !strcmp(optvalue, "argon2d")) {
                                        params.argon2_flavour = Argon2d;
                                    } else {
                                        errs = true;
                                        fprintf(stderr, "puttygen: 无法识别"
                                                "d kdf '%s'\n", optvalue);
                                    }
                                    continue;
                                }

                                char *end;
                                unsigned long n = strtoul(optvalue, &end, 0);
                                if (!*optvalue || *end) {
                                    errs = true;
                                    fprintf(stderr, "puttygen: '%s' 为"
                                            "PPK参数值 '%s': 应该是一个"
                                            "数字\n", optvalue, val);
                                    continue;
                                }

                                if (!strcmp(val, "version")) {
                                    params.fmt_version = n;
                                } else if (!strcmp(val, "memory") ||
                                           !strcmp(val, "mem")) {
                                    params.argon2_mem = n;
                                } else if (!strcmp(val, "time")) {
                                    params.argon2_passes_auto = true;
                                    params.argon2_milliseconds = n;
                                } else if (!strcmp(val, "passes")) {
                                    params.argon2_passes_auto = false;
                                    params.argon2_passes = n;
                                } else if (!strcmp(val, "parallelism") ||
                                           !strcmp(val, "parallel")) {
                                    params.argon2_parallelism = n;
                                } else {
                                    errs = true;
                                    fprintf(stderr, "puttygen: 无法识别的"
                                            "PPK参数 '%s'\n", val);
                                    continue;
                                }
                            }
                        }
                    } else {
                        errs = true;
                        fprintf(stderr,
                                "puttygen: 无效选项 `-%s'\n", opt);
                    }
                    p = NULL;
                    break;
                  }
                  case 'h':
                  case 'V':
                  case 'P':
                  case 'l':
                  case 'L':
                  case 'p':
                  case 'q':
                    /*
                     * Option requiring no parameter.
                     */
                    switch (c) {
                      case 'h':
                        help();
                        nogo = true;
                        break;
                      case 'V':
                        showversion();
                        nogo = true;
                        break;
                      case 'P':
                        change_passphrase = true;
                        break;
                      case 'l':
                        outtype = FP;
                        break;
                      case 'L':
                        outtype = PUBLICO;
                        break;
                      case 'p':
                        outtype = PUBLIC;
                        break;
                      case 'q':
                        progress_fp = NULL;
                        break;
                    }
                    break;
                  case 't':
                  case 'b':
                  case 'C':
                  case 'O':
                  case 'o':
                  case 'E':
                    /*
                     * Option requiring parameter.
                     */
                    p++;
                    if (!*p && argc > 1)
                        --argc, p = *++argv;
                    else if (!*p) {
                        fprintf(stderr, "puttygen: 选项 `-%c' 需要一个"
                                "参数\n", c);
                        errs = true;
                    }
                    /*
                     * Now c is the option and p is the parameter.
                     */
                    switch (c) {
                      case 't':
                        if (!strcmp(p, "rsa") || !strcmp(p, "rsa2"))
                            keytype = RSA2, sshver = 2;
                        else if (!strcmp(p, "rsa1"))
                            keytype = RSA1, sshver = 1;
                        else if (!strcmp(p, "dsa") || !strcmp(p, "dss"))
                            keytype = DSA, sshver = 2;
                        else if (!strcmp(p, "ecdsa"))
                            keytype = ECDSA, sshver = 2;
                        else if (!strcmp(p, "eddsa"))
                            keytype = EDDSA, sshver = 2;
                        else if (!strcmp(p, "ed25519"))
                            keytype = EDDSA, bits = 255, sshver = 2;
                        else if (!strcmp(p, "ed448"))
                            keytype = EDDSA, bits = 448, sshver = 2;
                        else {
                            fprintf(stderr,
                                    "puttygen: 未知密钥类型 `%s'\n", p);
                            errs = true;
                        }
                        break;
                      case 'b':
                        bits = atoi(p);
                        break;
                      case 'C':
                        comment = p;
                        break;
                      case 'O':
                        if (!strcmp(p, "public"))
                            outtype = PUBLIC;
                        else if (!strcmp(p, "public-openssh"))
                            outtype = PUBLICO;
                        else if (!strcmp(p, "private"))
                            outtype = PRIVATE;
                        else if (!strcmp(p, "fingerprint"))
                            outtype = FP;
                        else if (!strcmp(p, "private-openssh"))
                            outtype = OPENSSH_AUTO, sshver = 2;
                        else if (!strcmp(p, "private-openssh-new"))
                            outtype = OPENSSH_NEW, sshver = 2;
                        else if (!strcmp(p, "private-sshcom"))
                            outtype = SSHCOM, sshver = 2;
                        else if (!strcmp(p, "text"))
                            outtype = TEXT;
                        else if (!strcmp(p, "cert-info"))
                            outtype = CERTINFO;
                        else {
                            fprintf(stderr,
                                    "puttygen: 未知输出类型 `%s'\n", p);
                            errs = true;
                        }
                        break;
                      case 'o':
                        outfile = p;
                        break;
                      case 'E':
                        if (!strcmp(p, "md5"))
                            fptype = SSH_FPTYPE_MD5;
                        else if (!strcmp(p, "sha256"))
                            fptype = SSH_FPTYPE_SHA256;
                        else if (!strcmp(p, "md5-cert"))
                            fptype = SSH_FPTYPE_MD5_CERT;
                        else if (!strcmp(p, "sha256-cert"))
                            fptype = SSH_FPTYPE_SHA256_CERT;
                        else {
                            fprintf(stderr, "puttygen: 未知指纹"
                                    "类型 `%s'\n", p);
                            errs = true;
                        }
                        break;
                    }
                    p = NULL;          /* prevent continued processing */
                    break;
                  default:
                    /*
                     * Unrecognised option.
                     */
                    errs = true;
                    fprintf(stderr, "puttygen: 无效选项 `-%c'\n", c);
                    break;
                }
            }
        } else {
            /*
             * A non-option argument.
             */
            if (!infile)
                infile = p;
            else {
                errs = true;
                fprintf(stderr, "puttygen: 不能处理多个"
                         "输入文件\n");
            }
        }
    }

    if (bits == -1) {
        /*
         * No explicit key size was specified. Default varies
         * depending on key type.
         */
        switch (keytype) {
          case ECDSA:
            bits = 384;
            break;
          case EDDSA:
            bits = 255;
            break;
          default:
            bits = DEFAULT_RSADSA_BITS;
            break;
        }
    }

    if (keytype == ECDSA || keytype == EDDSA) {
        const char *name = (keytype == ECDSA ? "ECDSA" : "EdDSA");
        const int *valid_lengths = (keytype == ECDSA ? ec_nist_curve_lengths :
                                    ec_ed_curve_lengths);
        size_t n_lengths = (keytype == ECDSA ? n_ec_nist_curve_lengths :
                            n_ec_ed_curve_lengths);
        bool (*alg_and_curve_by_bits)(int, const struct ec_curve **,
                                      const ssh_keyalg **) =
            (keytype == ECDSA ? ec_nist_alg_and_curve_by_bits :
             ec_ed_alg_and_curve_by_bits);

        const struct ec_curve *curve;
        const ssh_keyalg *alg;

        if (!alg_and_curve_by_bits(bits, &curve, &alg)) {
            fprintf(stderr, "puttygen: 无效的位选择 %s", name);
            for (size_t i = 0; i < n_lengths; i++)
                fprintf(stderr, "%s%d", (i == 0 ? " " :
                                         i == n_lengths-1 ? " or " : ", "),
                        valid_lengths[i]);
            fputc('\n', stderr);
            errs = true;
        }
    }

    if (keytype == RSA2 || keytype == RSA1 || keytype == DSA) {
        if (bits < 256) {
            fprintf(stderr, "puttygen: 无法生成 %s 低于"
                    "256位的密钥\n", (keytype == DSA ? "DSA" : "RSA"));
            errs = true;
        } else if (bits < DEFAULT_RSADSA_BITS) {
            fprintf(stderr, "puttygen: 警告: %s 密钥低于"
                    " %d 位可能不安全\n",
                    (keytype == DSA ? "DSA" : "RSA"), DEFAULT_RSADSA_BITS);
            /* but this is just a warning, so proceed anyway */
        }
    }

    if (errs)
        RETURN(1);

    if (nogo)
        RETURN(0);

    /*
     * If run with at least one argument _but_ not the required
     * ones, print the usage message and return failure.
     */
    if (!infile && keytype == NOKEYGEN) {
        usage(true);
        RETURN(1);
    }

    /* ------------------------------------------------------------------
     * Figure out further details of exactly what we're going to do.
     */

    /*
     * Bomb out if we've been asked to both load and generate a
     * key.
     */
    if (keytype != NOKEYGEN && infile) {
        fprintf(stderr, "puttygen: 不能同时加载和生成密钥\n");
        RETURN(1);
    }

    /*
     * We must save the private part when generating a new key.
     */
    if (keytype != NOKEYGEN &&
        (outtype != PRIVATE && outtype != OPENSSH_AUTO &&
         outtype != OPENSSH_NEW && outtype != SSHCOM && outtype != TEXT)) {
        fprintf(stderr, "puttygen: 这将生成一个新密钥，但会"
                "丢弃私有部分\n");
        RETURN(1);
    }

    /*
     * Analyse the type of the input file, in case this affects our
     * course of action.
     */
    if (infile) {
        const char *load_error;

        infilename = filename_from_str(infile);
        if (!strcmp(infile, "-"))
            infile_lf = lf_load_keyfile_fp(stdin, &load_error);
        else
            infile_lf = lf_load_keyfile(infilename, &load_error);

        if (!infile_lf) {
            fprintf(stderr, "puttygen: 无法加载文件 `%s': %s\n",
                    infile, load_error);
            RETURN(1);
        }

        infile_bs = BinarySource_UPCAST(infile_lf);
        intype = key_type_s(infile_bs);
        BinarySource_REWIND(infile_bs);

        switch (intype) {
          case SSH_KEYTYPE_UNOPENABLE:
          case SSH_KEYTYPE_UNKNOWN:
            fprintf(stderr, "puttygen: 无法加载文件 `%s': %s\n",
                    infile, key_type_to_str(intype));
            RETURN(1);

          case SSH_KEYTYPE_SSH1:
          case SSH_KEYTYPE_SSH1_PUBLIC:
            if (sshver == 2) {
                fprintf(stderr, "puttygen: 从SSH-1到SSH-2的密钥的转换"
                        "不受支持\n");
                RETURN(1);
            }
            sshver = 1;
            break;

          case SSH_KEYTYPE_SSH2:
          case SSH_KEYTYPE_SSH2_PUBLIC_RFC4716:
          case SSH_KEYTYPE_SSH2_PUBLIC_OPENSSH:
          case SSH_KEYTYPE_OPENSSH_PEM:
          case SSH_KEYTYPE_OPENSSH_NEW:
          case SSH_KEYTYPE_SSHCOM:
            if (sshver == 1) {
                fprintf(stderr, "puttygen: 从SSH-2到SSH-1的密钥的转换"
                        "不受支持\n");
                RETURN(1);
            }
            sshver = 2;
            break;

          case SSH_KEYTYPE_OPENSSH_AUTO:
          default:
            unreachable("永远不应该在输入文件中看到这些类型");
        }
    }

    /*
     * Determine the default output file, if none is provided.
     *
     * This will usually be equal to stdout, except that if the
     * input and output file formats are the same then the default
     * output is to overwrite the input.
     *
     * Also in this code, we bomb out if the input and output file
     * formats are the same and no other action is performed.
     */
    if ((intype == SSH_KEYTYPE_SSH1 && outtype == PRIVATE) ||
        (intype == SSH_KEYTYPE_SSH2 && outtype == PRIVATE) ||
        (intype == SSH_KEYTYPE_OPENSSH_PEM && outtype == OPENSSH_AUTO) ||
        (intype == SSH_KEYTYPE_OPENSSH_NEW && outtype == OPENSSH_NEW) ||
        (intype == SSH_KEYTYPE_SSHCOM && outtype == SSHCOM)) {
        if (!outfile) {
            outfile = infile;
            outfiletmp = dupcat(outfile, ".tmp");
        }

        if (!change_passphrase && !comment && !reencrypt && !certfile &&
            !remove_cert) {
            fprintf(stderr, "puttygen: 此命令不会执行任何有用的"
                    "操作\n");
            RETURN(1);
        }
    } else {
        if (!outfile) {
            /*
             * Bomb out rather than automatically choosing to write
             * a private key file to stdout.
             */
            if (outtype == PRIVATE || outtype == OPENSSH_AUTO ||
                outtype == OPENSSH_NEW || outtype == SSHCOM) {
                fprintf(stderr, "puttygen: 需要指定输出文件\n");
                RETURN(1);
            }
        }
    }

    /*
     * Figure out whether we need to load the encrypted part of the
     * key. This will be the case if (a) we need to write out
     * a private key format, (b) the entire input key file is
     * encrypted, or (c) we're outputting TEXT, in which case we
     * want all of the input file including private material if it
     * exists.
     */
    bool intype_entirely_encrypted =
        intype == SSH_KEYTYPE_OPENSSH_PEM ||
        intype == SSH_KEYTYPE_OPENSSH_NEW ||
        intype == SSH_KEYTYPE_SSHCOM;
    bool intype_has_private =
        !(intype == SSH_KEYTYPE_SSH1_PUBLIC ||
          intype == SSH_KEYTYPE_SSH2_PUBLIC_RFC4716 ||
          intype == SSH_KEYTYPE_SSH2_PUBLIC_OPENSSH);
    bool outtype_has_private =
        outtype == PRIVATE || outtype == OPENSSH_AUTO ||
        outtype == OPENSSH_NEW || outtype == SSHCOM;
    if (outtype_has_private || intype_entirely_encrypted ||
        (outtype == TEXT && intype_has_private))
        load_encrypted = true;
    else
        load_encrypted = false;

    if (load_encrypted && !intype_has_private) {
        fprintf(stderr, "puttygen: 无法对单纯的公钥输入文件 "
                "执行此操作\n");
        RETURN(1);
    }

    /*
     * Check consistency properties relating to certificates.
     */
    if (certfile && !(sshver == 2 && intype_has_private &&
                      outtype_has_private && infile)) {
        fprintf(stderr, "puttygen: 证书只能添加到现有的 "
                "SSH-2 私钥文件\n");
        RETURN(1);
    }
    if (remove_cert && !(sshver == 2 && infile)) {
        fprintf(stderr, "puttygen: 只能从现有的 SSH-2 密钥文件"
                "中删除证书\n");
        RETURN(1);
    }
    if (certfile && remove_cert) {
        fprintf(stderr, "puttygen: 不能同时添加和"
                "删除证书\n");
        RETURN(1);
    }

    /* ------------------------------------------------------------------
     * Now we're ready to actually do some stuff.
     */

    /*
     * Either load or generate a key.
     */
    if (keytype != NOKEYGEN) {
        char *entropy;
        char default_comment[80];
        struct tm tm;

        tm = ltime();
        if (keytype == DSA)
            strftime(default_comment, 30, "dsa-key-%Y%m%d", &tm);
        else if (keytype == ECDSA)
            strftime(default_comment, 30, "ecdsa-key-%Y%m%d", &tm);
        else if (keytype == EDDSA && bits == 255)
            strftime(default_comment, 30, "ed25519-key-%Y%m%d", &tm);
        else if (keytype == EDDSA)
            strftime(default_comment, 30, "eddsa-key-%Y%m%d", &tm);
        else
            strftime(default_comment, 30, "rsa-key-%Y%m%d", &tm);

        entropy = get_random_data(bits / 8, random_device);
        if (!entropy) {
            fprintf(stderr, "puttygen: 无法收集entropy, "
                    "无法生成密钥\n");
            RETURN(1);
        }
        random_setup_special();
        random_reseed(make_ptrlen(entropy, bits / 8));
        smemclr(entropy, bits/8);
        sfree(entropy);

        PrimeGenerationContext *pgc = primegen_new_context(primegen);

        if (keytype == DSA) {
            struct dsa_key *dsakey = snew(struct dsa_key);
            dsa_generate(dsakey, bits, pgc, &cmdgen_progress);
            ssh2key = snew(ssh2_userkey);
            ssh2key->key = &dsakey->sshk;
            ssh1key = NULL;
        } else if (keytype == ECDSA) {
            struct ecdsa_key *ek = snew(struct ecdsa_key);
            ecdsa_generate(ek, bits);
            ssh2key = snew(ssh2_userkey);
            ssh2key->key = &ek->sshk;
            ssh1key = NULL;
        } else if (keytype == EDDSA) {
            struct eddsa_key *ek = snew(struct eddsa_key);
            eddsa_generate(ek, bits);
            ssh2key = snew(ssh2_userkey);
            ssh2key->key = &ek->sshk;
            ssh1key = NULL;
        } else {
            RSAKey *rsakey = snew(RSAKey);
            rsa_generate(rsakey, bits, strong_rsa, pgc, &cmdgen_progress);
            rsakey->comment = NULL;
            if (keytype == RSA1) {
                ssh1key = rsakey;
            } else {
                ssh2key = snew(ssh2_userkey);
                ssh2key->key = &rsakey->sshk;
            }
        }

        primegen_free_context(pgc);

        if (ssh2key)
            ssh2key->comment = dupstr(default_comment);
        if (ssh1key)
            ssh1key->comment = dupstr(default_comment);

    } else {
        const char *error = NULL;
        bool encrypted;

        assert(infile != NULL);

        sfree(origcomment);
        origcomment = NULL;

        /*
         * Find out whether the input key is encrypted.
         */
        if (intype == SSH_KEYTYPE_SSH1)
            encrypted = rsa1_encrypted_s(infile_bs, &origcomment);
        else if (intype == SSH_KEYTYPE_SSH2)
            encrypted = ppk_encrypted_s(infile_bs, &origcomment);
        else
            encrypted = import_encrypted_s(infilename, infile_bs,
                                           intype, &origcomment);
        BinarySource_REWIND(infile_bs);

        /*
         * If so, ask for a passphrase.
         */
        if (encrypted && load_encrypted) {
            if (!old_passphrase) {
                prompts_t *p = new_prompts();
                SeatPromptResult spr;
                p->to_server = false;
                p->from_server = false;
                p->name = dupstr("SSH 密钥密码");
                add_prompt(p, dupstr("输入密钥以加载密钥: "), false);
                spr = console_get_userpass_input(p);
                assert(spr.kind != SPRK_INCOMPLETE);
                if (spr_is_abort(spr)) {
                    free_prompts(p);
                    spr_error(spr);
                    RETURN(1);
                } else {
                    old_passphrase = prompt_get_result(p->prompts[0]);
                    free_prompts(p);
                }
            }
        } else {
            old_passphrase = NULL;
        }

        switch (intype) {
            int ret;

          case SSH_KEYTYPE_SSH1:
          case SSH_KEYTYPE_SSH1_PUBLIC:
            ssh1key = snew(RSAKey);
            memset(ssh1key, 0, sizeof(RSAKey));
            if (!load_encrypted) {
                strbuf *blob;
                BinarySource src[1];

                sfree(origcomment);
                origcomment = NULL;

                blob = strbuf_new();

                ret = rsa1_loadpub_s(infile_bs, BinarySink_UPCAST(blob),
                                     &origcomment, &error);
                BinarySource_BARE_INIT(src, blob->u, blob->len);
                get_rsa_ssh1_pub(src, ssh1key, RSA_SSH1_EXPONENT_FIRST);
                strbuf_free(blob);

                ssh1key->comment = dupstr(origcomment);
                ssh1key->private_exponent = NULL;
                ssh1key->p = NULL;
                ssh1key->q = NULL;
                ssh1key->iqmp = NULL;
            } else {
                ret = rsa1_load_s(infile_bs, ssh1key, old_passphrase, &error);
            }
            BinarySource_REWIND(infile_bs);
            if (ret > 0)
                error = NULL;
            else if (!error)
                error = "未知错误";
            break;

          case SSH_KEYTYPE_SSH2:
          case SSH_KEYTYPE_SSH2_PUBLIC_RFC4716:
          case SSH_KEYTYPE_SSH2_PUBLIC_OPENSSH:
            if (!load_encrypted) {
                sfree(origcomment);
                origcomment = NULL;
                ssh2blob = strbuf_new();
                if (ppk_loadpub_s(infile_bs, &ssh2alg,
                                  BinarySink_UPCAST(ssh2blob),
                                  &origcomment, &error)) {
                    const ssh_keyalg *alg = find_pubkey_alg(ssh2alg);
                    if (alg)
                        bits = ssh_key_public_bits(
                            alg, ptrlen_from_strbuf(ssh2blob));
                    else
                        bits = -1;
                } else {
                    strbuf_free(ssh2blob);
                    ssh2blob = NULL;
                }
                sfree(ssh2alg);
            } else {
                ssh2key = ppk_load_s(infile_bs, old_passphrase, &error);
            }
            BinarySource_REWIND(infile_bs);
            if ((ssh2key && ssh2key != SSH2_WRONG_PASSPHRASE) || ssh2blob)
                error = NULL;
            else if (!error) {
                if (ssh2key == SSH2_WRONG_PASSPHRASE)
                    error = "密码错误";
                else
                    error = "未知错误";
            }
            break;

          case SSH_KEYTYPE_OPENSSH_PEM:
          case SSH_KEYTYPE_OPENSSH_NEW:
          case SSH_KEYTYPE_SSHCOM:
            ssh2key = import_ssh2_s(infile_bs, intype, old_passphrase, &error);
            if (ssh2key) {
                if (ssh2key != SSH2_WRONG_PASSPHRASE)
                    error = NULL;
                else
                    error = "密码错误";
            } else if (!error)
                error = "未知错误";
            break;

          default:
            unreachable("输入密钥类型错误");
        }

        if (error) {
            fprintf(stderr, "puttygen: 加载出错 `%s': %s\n",
                    infile, error);
            RETURN(1);
        }
    }

    /*
     * Change the comment if asked to.
     */
    if (comment) {
        if (sshver == 1) {
            assert(ssh1key);
            sfree(ssh1key->comment);
            ssh1key->comment = dupstr(comment);
        } else {
            assert(ssh2key);
            sfree(ssh2key->comment);
            ssh2key->comment = dupstr(comment);
        }
    }

    /*
     * Swap out the public key for a different one, if asked to.
     */
    if (certfile) {
        Filename *certfilename = filename_from_str(certfile);
        LoadedFile *certfile_lf;
        const char *error = NULL;

        if (!strcmp(certfile, "-"))
            certfile_lf = lf_load_keyfile_fp(stdin, &error);
        else
            certfile_lf = lf_load_keyfile(certfilename, &error);

        filename_free(certfilename);

        if (!certfile_lf) {
            fprintf(stderr, "puttygen: 无法加载证书文件 `%s': "
                    "%s\n", certfile, error);
            RETURN(1);
        }

        char *algname = NULL;
        char *comment = NULL;
        strbuf *pub = strbuf_new();
        if (!ppk_loadpub_s(BinarySource_UPCAST(certfile_lf), &algname,
                           BinarySink_UPCAST(pub), &comment, &error)) {
            fprintf(stderr, "puttygen: 无法加载证书文件 `%s': "
                    "%s\n", certfile, error);
            strbuf_free(pub);
            sfree(algname);
            sfree(comment);
            lf_free(certfile_lf);
            RETURN(1);
        }

        lf_free(certfile_lf);
        sfree(comment);

        const ssh_keyalg *alg = find_pubkey_alg(algname);
        if (!alg) {
            fprintf(stderr, "puttygen: 证书文件 `%s' 已不受支持，"
                    "算法名称 `%s'\n", certfile, algname);
            strbuf_free(pub);
            sfree(algname);
            RETURN(1);
        }

        sfree(algname);

        /* Check the two public keys match apart from certificates */
        strbuf *old_basepub = strbuf_new();
        ssh_key_public_blob(ssh_key_base_key(ssh2key->key),
                            BinarySink_UPCAST(old_basepub));

        ssh_key *new_pubkey = ssh_key_new_pub(alg, ptrlen_from_strbuf(pub));
        strbuf *new_basepub = strbuf_new();
        ssh_key_public_blob(ssh_key_base_key(new_pubkey),
                            BinarySink_UPCAST(new_basepub));
        ssh_key_free(new_pubkey);

        bool match = ptrlen_eq_ptrlen(ptrlen_from_strbuf(old_basepub),
                                      ptrlen_from_strbuf(new_basepub));
        strbuf_free(old_basepub);
        strbuf_free(new_basepub);

        if (!match) {
            fprintf(stderr, "puttygen: `%s' 证书与"
                    " `%s' 中的公钥不匹配\n", certfile, infile);
            strbuf_free(pub);
            RETURN(1);
        }

        strbuf *priv = strbuf_new_nm();
        ssh_key_private_blob(ssh2key->key, BinarySink_UPCAST(priv));
        ssh_key *newkey = ssh_key_new_priv(
            alg, ptrlen_from_strbuf(pub), ptrlen_from_strbuf(priv));
        strbuf_free(pub);
        strbuf_free(priv);

        if (!newkey) {
            fprintf(stderr, "puttygen: 无法将 `%s' 中的证书与"
                    "私钥组合在一起\n", certfile);
            RETURN(1);
        }

        ssh_key_free(ssh2key->key);
        ssh2key->key = newkey;
    } else if (remove_cert) {
        /*
         * Removing a certificate can be meaningfully done to a pure
         * public key blob, as well as a full key pair.
         */
        if (ssh2key) {
            ssh_key *newkey = ssh_key_clone(ssh_key_base_key(ssh2key->key));
            ssh_key_free(ssh2key->key);
            ssh2key->key = newkey;
        } else if (ssh2blob) {
            ptrlen algname = pubkey_blob_to_alg_name(
                ptrlen_from_strbuf(ssh2blob));

            const ssh_keyalg *alg = find_pubkey_alg_len(algname);

            if (!alg) {
                fprintf(stderr, "puttygen: 输入文件 `%s' 已不支持"
                        "名称为 `%.*s'的算法\n", infile,
                        PTRLEN_PRINTF(algname));
                RETURN(1);
            }

            ssh_key *tmpkey = ssh_key_new_pub(
                alg, ptrlen_from_strbuf(ssh2blob));
            strbuf_clear(ssh2blob);
            ssh_key_public_blob(ssh_key_base_key(tmpkey),
                                BinarySink_UPCAST(ssh2blob));
            ssh_key_free(tmpkey);
        }
    }

    /*
     * Unless we're changing the passphrase, the old one (if any) is a
     * reasonable default.
     */
    if (!change_passphrase && old_passphrase && !new_passphrase)
        new_passphrase = dupstr(old_passphrase);

    /*
     * Prompt for a new passphrase if we have been asked to, or if
     * we have just generated a key.
     *
     * In the latter case, an exception is if we're producing text
     * output, because that output format doesn't support encryption
     * in any case.
     */
    if (!new_passphrase && (change_passphrase ||
                            (keytype != NOKEYGEN && outtype != TEXT))) {
        prompts_t *p = new_prompts();
        SeatPromptResult spr;

        p->to_server = false;
        p->from_server = false;
        p->name = dupstr("新的SSH密钥密码");
        add_prompt(p, dupstr("输入密码以保存密钥: "), false);
        add_prompt(p, dupstr("重新输入密码以验证: "), false);
        spr = console_get_userpass_input(p);
        assert(spr.kind != SPRK_INCOMPLETE);
        if (spr_is_abort(spr)) {
            free_prompts(p);
            spr_error(spr);
            RETURN(1);
        } else {
            if (strcmp(prompt_get_result_ref(p->prompts[0]),
                       prompt_get_result_ref(p->prompts[1]))) {
                free_prompts(p);
                fprintf(stderr, "puttygen: 密码不匹配\n");
                RETURN(1);
            }
            new_passphrase = prompt_get_result(p->prompts[0]);
            free_prompts(p);
        }
    }
    if (new_passphrase && !*new_passphrase) {
        sfree(new_passphrase);
        new_passphrase = NULL;
    }

    /*
     * Write output.
     *
     * (In the case where outfile and outfiletmp are both NULL,
     * there is no semantic reason to initialise outfilename at
     * all; but we have to write _something_ to it or some compiler
     * will probably complain that it might be used uninitialised.)
     */
    if (outfiletmp)
        outfilename = filename_from_str(outfiletmp);
    else
        outfilename = filename_from_str(outfile ? outfile : "");

    switch (outtype) {
        bool ret;
        int real_outtype;

      case PRIVATE:
        random_ref(); /* we'll need a few random bytes in the save file */
        if (sshver == 1) {
            assert(ssh1key);
            ret = rsa1_save_f(outfilename, ssh1key, new_passphrase);
            if (!ret) {
                fprintf(stderr, "puttygen: 无法保存SSH-1私钥\n");
                RETURN(1);
            }
        } else {
            assert(ssh2key);
            ret = ppk_save_f(outfilename, ssh2key, new_passphrase, &params);
            if (!ret) {
                fprintf(stderr, "puttygen: 无法保存SSH-2私钥\n");
                RETURN(1);
            }
        }
        if (outfiletmp) {
            if (!move(outfiletmp, outfile))
                RETURN(1);              /* rename failed */
        }
        break;

      case PUBLIC:
      case PUBLICO: {
        FILE *fp;

        if (outfile) {
            fp = f_open(outfilename, "w", false);
            if (!fp) {
                fprintf(stderr, "无法打开输出文件\n");
                exit(1);
            }
        } else {
            fp = stdout;
        }

        if (sshver == 1) {
            ssh1_write_pubkey(fp, ssh1key);
        } else {
            if (!ssh2blob) {
                assert(ssh2key);
                ssh2blob = strbuf_new();
                ssh_key_public_blob(ssh2key->key, BinarySink_UPCAST(ssh2blob));
            }

            ssh2_write_pubkey(fp, ssh2key ? ssh2key->comment : origcomment,
                              ssh2blob->s, ssh2blob->len,
                              (outtype == PUBLIC ?
                               SSH_KEYTYPE_SSH2_PUBLIC_RFC4716 :
                               SSH_KEYTYPE_SSH2_PUBLIC_OPENSSH));
        }

        if (outfile)
            fclose(fp);

        break;
      }

      case FP: {
        FILE *fp;
        char *fingerprint;

        if (sshver == 1) {
            assert(ssh1key);
            fingerprint = rsa_ssh1_fingerprint(ssh1key);
        } else {
            if (ssh2key) {
                fingerprint = ssh2_fingerprint(ssh2key->key, fptype);
            } else {
                assert(ssh2blob);
                fingerprint = ssh2_fingerprint_blob(
                    ptrlen_from_strbuf(ssh2blob), fptype);
            }
        }

        if (outfile) {
            fp = f_open(outfilename, "w", false);
            if (!fp) {
                fprintf(stderr, "无法打开输出文件\n");
                exit(1);
            }
        } else {
            fp = stdout;
        }
        fprintf(fp, "%s\n", fingerprint);
        if (outfile)
            fclose(fp);

        sfree(fingerprint);
        break;
      }

      case OPENSSH_AUTO:
      case OPENSSH_NEW:
      case SSHCOM:
        assert(sshver == 2);
        assert(ssh2key);
        random_ref(); /* both foreign key types require randomness,
                       * for IV or padding */
        switch (outtype) {
          case OPENSSH_AUTO:
            real_outtype = SSH_KEYTYPE_OPENSSH_AUTO;
            break;
          case OPENSSH_NEW:
            real_outtype = SSH_KEYTYPE_OPENSSH_NEW;
            break;
          case SSHCOM:
            real_outtype = SSH_KEYTYPE_SSHCOM;
            break;
          default:
            unreachable("控制流错误");
        }
        ret = export_ssh2(outfilename, real_outtype, ssh2key, new_passphrase);
        if (!ret) {
            fprintf(stderr, "puttygen: 无法导出密钥\n");
            RETURN(1);
        }
        if (outfiletmp) {
            if (!move(outfiletmp, outfile))
                RETURN(1);              /* rename failed */
        }
        break;

      case TEXT: {
        key_components *kc;
        if (sshver == 1) {
            assert(ssh1key);
            kc = rsa_components(ssh1key);
        } else {
            if (ssh2key) {
                kc = ssh_key_components(ssh2key->key);
            } else {
                assert(ssh2blob);

                ptrlen algname = pubkey_blob_to_alg_name(
                    ptrlen_from_strbuf(ssh2blob));
                const ssh_keyalg *alg = find_pubkey_alg_len(algname);
                if (!alg) {
                    fprintf(stderr, "puttygen: 无法取得密钥组件，"
                            "来自未知类型的公钥 '%.*s'\n",
                            PTRLEN_PRINTF(algname));
                    RETURN(1);
                }
                ssh_key *sk = ssh_key_new_pub(
                    alg, ptrlen_from_strbuf(ssh2blob));
                if (!sk) {
                    fprintf(stderr, "puttygen: 无法解码公钥\n");
                    RETURN(1);
                }
                kc = ssh_key_components(sk);
                ssh_key_free(sk);
            }
        }

        FILE *fp;
        if (outfile) {
            fp = f_open(outfilename, "w", false);
            if (!fp) {
                fprintf(stderr, "无法打开输出文件\n");
                exit(1);
            }
        } else {
            fp = stdout;
        }

        for (size_t i = 0; i < kc->ncomponents; i++) {
            key_component *comp = &kc->components[i];
            fprintf(fp, "%s=", comp->name);
            switch (comp->type) {
              case KCT_MPINT: {
                char *hex = mp_get_hex(comp->mp);
                fprintf(fp, "0x%s\n", hex);
                smemclr(hex, strlen(hex));
                sfree(hex);
                break;
              }
              case KCT_TEXT:
                fputs("\"", fp);
                write_c_string_literal(fp, ptrlen_from_strbuf(comp->str));
                fputs("\"\n", fp);
                break;
              case KCT_BINARY: {
                /*
                 * Display format for binary key components is to show
                 * them as base64, with a wrapper so that the actual
                 * printed string is along the lines of
                 * 'b64("aGVsbG8sIHdvcmxkCg==")'.
                 *
                 * That's a compromise between not being too verbose
                 * for a human reader, and still being reasonably
                 * friendly to people pasting the output of this
                 * 'puttygen --dump' option into Python code (which
                 * the format is designed to permit in general).
                 *
                 * Python users pasting a dump containing one of these
                 * will have to define a function 'b64' in advance
                 * which takes a string, which you can do most easily
                 * using this import statement, as seen in
                 * cryptsuite.py:
                 *
                 * from base64 import b64decode as b64
                 */
                fputs("b64(\"", fp);
                char b64[4];
                for (size_t j = 0; j < comp->str->len; j += 3) {
                    size_t len = comp->str->len - j;
                    if (len > 3) len = 3;
                    base64_encode_atom(comp->str->u + j, len, b64);
                    fwrite(b64, 1, 4, fp);
                }
                fputs("\")\n", fp);
                break;
              }
              default:
                unreachable("错误的密钥组件类型");
            }
        }

        if (outfile)
            fclose(fp);
        key_components_free(kc);
        break;
      }

      case CERTINFO: {
        if (sshver == 1) {
            fprintf(stderr, "puttygen: SSH-1 密钥不能"
                    "包含证书\n");
            RETURN(1);
        }

        const ssh_keyalg *alg;
        ssh_key *sk;
        bool sk_allocated = false;

        if (ssh2key) {
            sk = ssh2key->key;
            alg = ssh_key_alg(sk);
        } else {
            assert(ssh2blob);
            ptrlen algname = pubkey_blob_to_alg_name(
                ptrlen_from_strbuf(ssh2blob));
            alg = find_pubkey_alg_len(algname);
            if (!alg) {
                fprintf(stderr, "puttygen: '%.*s' 公钥类型未知，"
                        "无法提取证书信息\n",
                        PTRLEN_PRINTF(algname));
                RETURN(1);
            }
            sk = ssh_key_new_pub(alg, ptrlen_from_strbuf(ssh2blob));
            if (!sk) {
                fprintf(stderr, "puttygen: 无法解码公钥\n");
                RETURN(1);
            }
            sk_allocated = true;
        }

        if (!alg->is_certificate) {
            fprintf(stderr, "puttygen: 密钥不是证书\n");
        } else {
            SeatDialogText *text = ssh_key_cert_info(sk);

            FILE *fp;
            if (outfile) {
                fp = f_open(outfilename, "w", false);
                if (!fp) {
                    fprintf(stderr, "无法打开输出文件\n");
                    exit(1);
                }
            } else {
                fp = stdout;
            }

            for (SeatDialogTextItem *item = text->items,
                     *end = item+text->nitems; item < end; item++) {
                switch (item->type) {
                  case SDT_MORE_INFO_KEY:
                    fprintf(fp, "%s", item->text);
                    break;
                  case SDT_MORE_INFO_VALUE_SHORT:
                    fprintf(fp, ": %s\n", item->text);
                    break;
                  case SDT_MORE_INFO_VALUE_BLOB:
                    fprintf(fp, ":\n%s\n", item->text);
                    break;
                  default:
                    break;
                }
            }

            if (outfile)
                fclose(fp);

            seat_dialog_text_free(text);
        }

        if (sk_allocated)
            ssh_key_free(sk);
        break;
      }
    }

  out:

    #undef RETURN

    if (old_passphrase) {
        smemclr(old_passphrase, strlen(old_passphrase));
        sfree(old_passphrase);
    }
    if (new_passphrase) {
        smemclr(new_passphrase, strlen(new_passphrase));
        sfree(new_passphrase);
    }

    if (ssh1key) {
        freersakey(ssh1key);
        sfree(ssh1key);
    }
    if (ssh2key && ssh2key != SSH2_WRONG_PASSPHRASE) {
        sfree(ssh2key->comment);
        if (ssh2key->key)
            ssh_key_free(ssh2key->key);
        sfree(ssh2key);
    }
    if (ssh2blob)
        strbuf_free(ssh2blob);
    sfree(origcomment);
    if (infilename)
        filename_free(infilename);
    if (infile_lf)
        lf_free(infile_lf);
    if (outfilename)
        filename_free(outfilename);
    sfree(outfiletmp);

    return exit_status;
}
