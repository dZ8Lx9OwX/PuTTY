/*
 * config.c - the Windows-specific parts of the PuTTY configuration
 * box.
 */

#include <assert.h>
#include <stdlib.h>

#include "putty.h"
#include "dialog.h"
#include "storage.h"

static void about_handler(dlgcontrol *ctrl, dlgparam *dlg,
                          void *data, int event)
{
    HWND *hwndp = (HWND *)ctrl->context.p;

    if (event == EVENT_ACTION) {
        modal_about_box(*hwndp);
    }
}

static void help_handler(dlgcontrol *ctrl, dlgparam *dlg,
                         void *data, int event)
{
    HWND *hwndp = (HWND *)ctrl->context.p;

    if (event == EVENT_ACTION) {
        show_help(*hwndp);
    }
}

static void variable_pitch_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event)
{
    if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg, !dlg_get_fixed_pitch_flag(dlg));
    } else if (event == EVENT_VALCHANGE) {
        dlg_set_fixed_pitch_flag(dlg, !dlg_checkbox_get(ctrl, dlg));
    }
}

void win_setup_config_box(struct controlbox *b, HWND *hwndp, bool has_help,
                          bool midsession, int protocol)
{
    const struct BackendVtable *backvt;
    bool resize_forbidden = false;
    struct controlset *s;
    dlgcontrol *c;
    char *str;

    if (!midsession) {
        /*
         * Add the About and Help buttons to the standard panel.
         */
        s = ctrl_getset(b, "", "", "");
        c = ctrl_pushbutton(s, "关于", 'a', HELPCTX(no_help),
                            about_handler, P(hwndp));
        c->column = 0;
        if (has_help) {
            c = ctrl_pushbutton(s, "帮助", 'h', HELPCTX(no_help),
                                help_handler, P(hwndp));
            c->column = 1;
        }
    }

    /*
     * Full-screen mode is a Windows peculiarity; hence
     * scrollbar_in_fullscreen is as well.
     */
    s = ctrl_getset(b, "窗口", "scrollback",
                    "窗口滚动设置：");
    ctrl_checkbox(s, "全屏模式下显示滚动条", 'i',
                  HELPCTX(window_scrollback),
                  conf_checkbox_handler,
                  I(CONF_scrollbar_in_fullscreen));
    /*
     * Really this wants to go just after `Display scrollbar'. See
     * if we can find that control, and do some shuffling.
     */
    {
        int i;
        for (i = 0; i < s->ncontrols; i++) {
            c = s->ctrls[i];
            if (c->type == CTRL_CHECKBOX &&
                c->context.i == CONF_scrollbar) {
                /*
                 * Control i is the scrollbar checkbox.
                 * Control s->ncontrols-1 is the scrollbar-in-FS one.
                 */
                if (i < s->ncontrols-2) {
                    c = s->ctrls[s->ncontrols-1];
                    memmove(s->ctrls+i+2, s->ctrls+i+1,
                            (s->ncontrols-i-2)*sizeof(dlgcontrol *));
                    s->ctrls[i+1] = c;
                }
                break;
            }
        }
    }

    /*
     * Windows has the AltGr key, which has various Windows-
     * specific options.
     */
    s = ctrl_getset(b, "终端/键盘", "features",
                    "启用额外的键盘功能：");
    ctrl_checkbox(s, "AltGr充当Compose键", 't',
                  HELPCTX(keyboard_compose),
                  conf_checkbox_handler, I(CONF_compose_key));
    ctrl_checkbox(s, "Control-Alt与AltGr不同", 'd',
                  HELPCTX(keyboard_ctrlalt),
                  conf_checkbox_handler, I(CONF_ctrlaltkeys));

    /*
     * Windows allows an arbitrary .WAV to be played as a bell, and
     * also the use of the PC speaker. For this we must search the
     * existing controlset for the radio-button set controlling the
     * `beep' option, and add extra buttons to it.
     *
     * Note that although this _looks_ like a hideous hack, it's
     * actually all above board. The well-defined interface to the
     * per-platform dialog box code is the _data structures_ `union
     * control', `struct controlset' and so on; so code like this
     * that reaches into those data structures and changes bits of
     * them is perfectly legitimate and crosses no boundaries. All
     * the ctrl_* routines that create most of the controls are
     * convenient shortcuts provided on the cross-platform side of
     * the interface, and template creation code is under no actual
     * obligation to use them.
     */
    s = ctrl_getset(b, "终端/提示音", "style", "自定义设置");
    {
        int i;
        for (i = 0; i < s->ncontrols; i++) {
            c = s->ctrls[i];
            if (c->type == CTRL_RADIO &&
                c->context.i == CONF_beep) {
                assert(c->handler == conf_radiobutton_handler);
                c->radio.nbuttons += 2;
                c->radio.buttons =
                    sresize(c->radio.buttons, c->radio.nbuttons, char *);
                c->radio.buttons[c->radio.nbuttons-1] =
                    dupstr("播放自定义提示音");
                c->radio.buttons[c->radio.nbuttons-2] =
                    dupstr("PC蜂鸣器提示音");
                c->radio.buttondata =
                    sresize(c->radio.buttondata, c->radio.nbuttons, intorptr);
                c->radio.buttondata[c->radio.nbuttons-1] = I(BELL_WAVEFILE);
                c->radio.buttondata[c->radio.nbuttons-2] = I(BELL_PCSPEAKER);
                if (c->radio.shortcuts) {
                    c->radio.shortcuts =
                        sresize(c->radio.shortcuts, c->radio.nbuttons, char);
                    c->radio.shortcuts[c->radio.nbuttons-1] = NO_SHORTCUT;
                    c->radio.shortcuts[c->radio.nbuttons-2] = NO_SHORTCUT;
                }
                break;
            }
        }
    }
    ctrl_filesel(s, "自定义提示音文件：", NO_SHORTCUT,
                 FILTER_WAVE_FILES, false, "选择声音文件",
                 HELPCTX(bell_style),
                 conf_filesel_handler, I(CONF_bell_wavefile));

    /*
     * While we've got this box open, taskbar flashing on a bell is
     * also Windows-specific.
     */
    ctrl_radiobuttons(s, "任务栏/标题栏提示音标志：", 'i', 3,
                      HELPCTX(bell_taskbar),
                      conf_radiobutton_handler,
                      I(CONF_beep_ind),
                      "已禁用", I(B_IND_DISABLED),
                      "闪烁", I(B_IND_FLASH),
                      "常规", I(B_IND_STEADY));

    /*
     * The sunken-edge border is a Windows GUI feature.
     */
    s = ctrl_getset(b, "窗口/外观", "border",
                    "调整窗口边框：");
    ctrl_checkbox(s, "下沉边框边缘(稍厚)", 's',
                  HELPCTX(appearance_border),
                  conf_checkbox_handler, I(CONF_sunken_edge));

    /*
     * Configurable font quality settings for Windows.
     */
    s = ctrl_getset(b, "窗口/外观", "font",
                    "字体设置：");
    ctrl_checkbox(s, "允许选择可变间距字体", NO_SHORTCUT,
                  HELPCTX(appearance_font), variable_pitch_handler, I(0));
    ctrl_radiobuttons(s, "字体效果", 'q', 2,
                      HELPCTX(appearance_font),
                      conf_radiobutton_handler,
                      I(CONF_font_quality),
                      "抗锯齿", I(FQ_ANTIALIASED),
                      "无抗锯齿", I(FQ_NONANTIALIASED),
                      "ClearType", I(FQ_CLEARTYPE),
                      "默认", I(FQ_DEFAULT));

    /*
     * Cyrillic Lock is a horrid misfeature even on Windows, and
     * the least we can do is ensure it never makes it to any other
     * platform (at least unless someone fixes it!).
     */
    s = ctrl_getset(b, "窗口/字符转换", "tweaks", NULL);
    ctrl_checkbox(s, "大写锁定键用于Cyrillic切换", 's',
                  HELPCTX(translation_cyrillic),
                  conf_checkbox_handler,
                  I(CONF_xlat_capslockcyr));

    /*
     * On Windows we can use but not enumerate translation tables
     * from the operating system. Briefly document this.
     */
    s = ctrl_getset(b, "窗口/字符转换", "trans",
                    "接收数据的字符集转换");
    ctrl_text(s, "(Windows支持但未列出的字符集,"
              "比如很多系统上都有的CP866,可以手动输入)",
              HELPCTX(translation_codepage));

    /*
     * Windows has the weird OEM font mode, which gives us some
     * additional options when working with line-drawing
     * characters.
     */
    str = dupprintf("调整%s处理字符的方式：", appname);
    s = ctrl_getset(b, "窗口/字符转换", "linedraw", str);
    sfree(str);
    {
        int i;
        for (i = 0; i < s->ncontrols; i++) {
            c = s->ctrls[i];
            if (c->type == CTRL_RADIO &&
                c->context.i == CONF_vtmode) {
                assert(c->handler == conf_radiobutton_handler);
                c->radio.nbuttons += 3;
                c->radio.buttons =
                    sresize(c->radio.buttons, c->radio.nbuttons, char *);
                c->radio.buttons[c->radio.nbuttons-3] =
                    dupstr("X Windows 画线绘制");
                c->radio.buttons[c->radio.nbuttons-2] =
                    dupstr("ANSI/OEM 模式画线");
                c->radio.buttons[c->radio.nbuttons-1] =
                    dupstr("仅OEM模式编码绘制");
                c->radio.buttondata =
                    sresize(c->radio.buttondata, c->radio.nbuttons, intorptr);
                c->radio.buttondata[c->radio.nbuttons-3] = I(VT_XWINDOWS);
                c->radio.buttondata[c->radio.nbuttons-2] = I(VT_OEMANSI);
                c->radio.buttondata[c->radio.nbuttons-1] = I(VT_OEMONLY);
                if (!c->radio.shortcuts) {
                    int j;
                    c->radio.shortcuts = snewn(c->radio.nbuttons, char);
                    for (j = 0; j < c->radio.nbuttons; j++)
                        c->radio.shortcuts[j] = NO_SHORTCUT;
                } else {
                    c->radio.shortcuts = sresize(c->radio.shortcuts,
                                                 c->radio.nbuttons, char);
                }
                c->radio.shortcuts[c->radio.nbuttons-3] = 'x';
                c->radio.shortcuts[c->radio.nbuttons-2] = 'b';
                c->radio.shortcuts[c->radio.nbuttons-1] = 'e';
                break;
            }
        }
    }

    /*
     * RTF paste is Windows-specific.
     */
    s = ctrl_getset(b, "窗口/选择/复制", "format",
                    "复制字符的方式：");
    ctrl_checkbox(s, "以RTF和纯文本格式复制", 'f',
                  HELPCTX(copy_rtf),
                  conf_checkbox_handler, I(CONF_rtf_paste));

    /*
     * Windows often has no middle button, so we supply a selection
     * mode in which the more critical Paste action is available on
     * the right button instead.
     */
    s = ctrl_getset(b, "窗口/选择", "mouse",
                    "鼠标的使用：");
    ctrl_radiobuttons(s, "鼠标按键动作", 'm', 1,
                      HELPCTX(selection_buttons),
                      conf_radiobutton_handler,
                      I(CONF_mouse_is_xterm),
                      "Windows--中键扩展,右键菜单", I(2),
                      "Compromise--中键扩展,右键粘贴", I(0),
                      "xterm--右键扩展,中键粘贴", I(1));
    /*
     * This really ought to go at the _top_ of its box, not the
     * bottom, so we'll just do some shuffling now we've set it
     * up...
     */
    c = s->ctrls[s->ncontrols-1];      /* this should be the new control */
    memmove(s->ctrls+1, s->ctrls, (s->ncontrols-1)*sizeof(dlgcontrol *));
    s->ctrls[0] = c;

    /*
     * Logical palettes don't even make sense anywhere except Windows.
     */
    s = ctrl_getset(b, "窗口/颜色", "general",
                    "颜色使用的常规选项");
    ctrl_checkbox(s, "尝试使用逻辑调色板", 'l',
                  HELPCTX(colours_logpal),
                  conf_checkbox_handler, I(CONF_try_palette));
    ctrl_checkbox(s, "使用系统颜色", 's',
                  HELPCTX(colours_system),
                  conf_checkbox_handler, I(CONF_system_colour));


    /*
     * Resize-by-changing-font is a Windows insanity.
     */

    backvt = backend_vt_from_proto(protocol);
    if (backvt)
        resize_forbidden = (backvt->flags & BACKEND_RESIZE_FORBIDDEN);
    if (!midsession || !resize_forbidden) {
        s = ctrl_getset(b, "窗口", "size", "设置窗口大小");
        ctrl_radiobuttons(s, "调整窗口大小时：", 'z', 1,
                          HELPCTX(window_resize),
                          conf_radiobutton_handler,
                          I(CONF_resize_action),
                          "更改行列数", I(RESIZE_TERM),
                          "更改字体大小", I(RESIZE_FONT),
                          "仅在最大化时更改字体大小", I(RESIZE_EITHER),
                          "完全禁止调整大小", I(RESIZE_DISABLED));
    }

    /*
     * Most of the Window/Behaviour stuff is there to mimic Windows
     * conventions which PuTTY can optionally disregard. Hence,
     * most of these options are Windows-specific.
     */
    s = ctrl_getset(b, "窗口/行为", "main", NULL);
    ctrl_checkbox(s, "ALT-F4 关闭窗口", '4',
                  HELPCTX(behaviour_altf4),
                  conf_checkbox_handler, I(CONF_alt_f4));
    ctrl_checkbox(s, "ALT-Space 显示系统菜单", 'y',
                  HELPCTX(behaviour_altspace),
                  conf_checkbox_handler, I(CONF_alt_space));
    ctrl_checkbox(s, "ALT 显示系统菜单", 'l',
                  HELPCTX(behaviour_altonly),
                  conf_checkbox_handler, I(CONF_alt_only));
    ctrl_checkbox(s, "窗口总是保持在顶部", 'e',
                  HELPCTX(behaviour_alwaysontop),
                  conf_checkbox_handler, I(CONF_alwaysontop));
    ctrl_checkbox(s, "Alt-Enter 开启全屏", 'f',
                  HELPCTX(behaviour_altenter),
                  conf_checkbox_handler,
                  I(CONF_fullscreenonaltenter));

    /*
     * Windows supports a local-command proxy.
     */
    if (!midsession) {
        int i;
        s = ctrl_getset(b, "连接/代理", "basics", NULL);
        for (i = 0; i < s->ncontrols; i++) {
            c = s->ctrls[i];
            if (c->type == CTRL_LISTBOX &&
                c->handler == proxy_type_handler) {
                c->context.i |= PROXY_UI_FLAG_LOCAL;
                break;
            }
        }
    }

    /*
     * $XAUTHORITY is not reliable on Windows, so we provide a
     * means to override it.
     */
    if (!midsession && backend_vt_from_proto(PROT_SSH)) {
        s = ctrl_getset(b, "连接/SSH/X11", "x11", "X11转发");
        ctrl_filesel(s, "用于本地显示的X授权文件：", 't',
                     NULL, false, "选择授权文件",
                     HELPCTX(ssh_tunnels_xauthority),
                     conf_filesel_handler, I(CONF_xauthfile));
    }
}
