/*
 * config-gtk.c - the GTK-specific parts of the PuTTY configuration
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
    if (event == EVENT_ACTION) {
        about_box(ctrl->context.p);
    }
}

void gtk_setup_config_box(struct controlbox *b, bool midsession, void *win)
{
    struct controlset *s, *s2;
    dlgcontrol *c;
    int i;

    if (!midsession) {
        /*
         * Add the About button to the standard panel.
         */
        s = ctrl_getset(b, "", "", "");
        c = ctrl_pushbutton(s, "关于", 'a', HELPCTX(no_help),
                            about_handler, P(win));
        c->column = 0;
    }

    /*
     * GTK makes it rather easier to put the scrollbar on the left
     * than Windows does!
     */
    s = ctrl_getset(b, "窗口", "scrollback",
                    "窗口滚动设置：");
    ctrl_checkbox(s, "左侧滚动条", 'l',
                  HELPCTX(no_help),
                  conf_checkbox_handler,
                  I(CONF_scrollbar_on_left));
    /*
     * Really this wants to go just after `Display scrollbar'. See
     * if we can find that control, and do some shuffling.
     */
    for (i = 0; i < s->ncontrols; i++) {
        c = s->ctrls[i];
        if (c->type == CTRL_CHECKBOX &&
            c->context.i == CONF_scrollbar) {
            /*
             * Control i is the scrollbar checkbox.
             * Control s->ncontrols-1 is the scrollbar-on-left one.
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

    /*
     * X requires three more fonts: bold, wide, and wide-bold; also
     * we need the fiddly shadow-bold-offset control. This would
     * make the Window/Appearance panel rather unwieldy and large,
     * so I think the sensible thing here is to _move_ this
     * controlset into a separate Window/Fonts panel!
     */
    s2 = ctrl_getset(b, "窗口/外观", "font",
                     "字体设置：");
    /* Remove this controlset from b. */
    for (i = 0; i < b->nctrlsets; i++) {
        if (b->ctrlsets[i] == s2) {
            memmove(b->ctrlsets+i, b->ctrlsets+i+1,
                    (b->nctrlsets-i-1) * sizeof(*b->ctrlsets));
            b->nctrlsets--;
            ctrl_free_set(s2);
            break;
        }
    }
    ctrl_settitle(b, "窗口/字体", "字体设置");
    s = ctrl_getset(b, "窗口/字体", "font",
                    "用于显示非粗体文本的字体：");
    ctrl_fontsel(s, "普通文本的字体", 'f',
                 HELPCTX(no_help),
                 conf_fontsel_handler, I(CONF_font));
    ctrl_fontsel(s, "宽文本(CJK)的字体", 'w',
                 HELPCTX(no_help),
                 conf_fontsel_handler, I(CONF_widefont));
    s = ctrl_getset(b, "窗口/字体", "fontbold",
                    "用于显示粗体文本的字体：");
    ctrl_fontsel(s, "粗体普通文本的字体", 'b',
                 HELPCTX(no_help),
                 conf_fontsel_handler, I(CONF_boldfont));
    ctrl_fontsel(s, "粗体宽文本(CJK)的字体", 'i',
                 HELPCTX(no_help),
                 conf_fontsel_handler, I(CONF_wideboldfont));
    ctrl_checkbox(s, "使用阴影粗体而不是字体粗体", 'u',
                  HELPCTX(no_help),
                  conf_checkbox_handler,
                  I(CONF_shadowbold));
    ctrl_text(s, "(请注意，仅当您未设置通过改变"
              "文本颜色来突出粗体时，才会"
              "使用字体粗体或阴影粗体.)",
              HELPCTX(no_help));
    ctrl_editbox(s, "阴影粗体的水平偏移量", 'z', 20,
                 HELPCTX(no_help), conf_editbox_handler,
                 I(CONF_shadowboldoffset), ED_INT);

    /*
     * Markus Kuhn feels, not totally unreasonably, that it's good
     * for all applications to shift into UTF-8 mode if they notice
     * that they've been started with a LANG setting dictating it,
     * so that people don't have to keep remembering a separate
     * UTF-8 option for every application they use. Therefore,
     * here's an override option in the Translation panel.
     */
    s = ctrl_getset(b, "窗口/字符转换", "trans",
                    "接收到的数据的字符集转换");
    ctrl_checkbox(s, "使用UTF-8覆盖本地设置", 'l',
                  HELPCTX(translation_utf8_override),
                  conf_checkbox_handler,
                  I(CONF_utf8_override));

#ifdef OSX_META_KEY_CONFIG
    /*
     * On OS X, there are multiple reasonable opinions about whether
     * Option or Command (or both, or neither) should act as a Meta
     * key, or whether they should have their normal OS functions.
     */
    s = ctrl_getset(b, "终端/键盘", "meta",
                    "选择Meta键：");
    ctrl_checkbox(s, "选项键充当Meta", 'p',
                  HELPCTX(no_help),
                  conf_checkbox_handler, I(CONF_osx_option_meta));
    ctrl_checkbox(s, "命令键充Meta", 'm',
                  HELPCTX(no_help),
                  conf_checkbox_handler, I(CONF_osx_command_meta));
#endif

    if (!midsession) {
        /*
         * Allow the user to specify the window class as part of the saved
         * configuration, so that they can have their window manager treat
         * different kinds of PuTTY and pterm differently if they want to.
         */
        s = ctrl_getset(b, "窗口/行为", "x11",
                        "X Window系统设置：");
        ctrl_editbox(s, "窗口类名", 'z', 50,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_winclass), ED_STR);
    }
}
