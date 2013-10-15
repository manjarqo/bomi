#include "trayicon.hpp"
#include <glib-object.h>


enum AppIndicatorCategory {APP_INDICATOR_CATEGORY_APPLICATION_STATUS};
enum AppIndicatorStatus {
 /*< prefix=APP_INDICATOR_STATUS >*/
    APP_INDICATOR_STATUS_PASSIVE, /*< nick=Passive >*/
    APP_INDICATOR_STATUS_ACTIVE, /*< nick=Active >*/
    APP_INDICATOR_STATUS_ATTENTION /*< nick=NeedsAttention >*/
};

using AppIndicator = void;
using GtkWidget = void;
using GtkMenu = void;
using GtkMenuShell = void;
using f_app_indicator_new = AppIndicator*(*)(const char *id, const char *icon_name, AppIndicatorCategory category);
using f_app_indicator_set_status = void(*)(AppIndicator *self, AppIndicatorStatus status);
using f_app_indicator_set_menu = void(*)(AppIndicator *self, GtkMenu *menu);
using f_gtk_menu_new = GtkWidget*(*)();
using f_gtk_menu_item_new_with_label = GtkWidget*(*)(const char *label);
using f_gtk_menu_shell_append = void(*)(GtkMenuShell *menu_shell, GtkWidget *child);
using f_gtk_widget_show = void(*)(GtkWidget *widget);

static f_app_indicator_set_status app_indicator_set_status = nullptr;

#define GTK_MENU_SHELL(a) a
#define GTK_MENU(a) a

struct TrayIcon::Data {
	bool unity = false;
	QSystemTrayIcon *tray = nullptr;
    AppIndicator *indicator = nullptr;
    GtkMenu *gmenu = nullptr;
};

TrayIcon::TrayIcon(const QIcon &icon, QObject *parent)
: QObject(parent), d(new Data) {
	d->unity = qgetenv("XDG_CURRENT_DESKTOP").toLower() == "unity";
    if (d->unity) {
        qDebug() << "DE is Unity. Fallback to AppIndicator instead of QSytemTrayIcon";
        QLibrary gtk(_L("gtk-x11-2.0"), 0);
#define DEC_FUNC(name) auto name = (f_##name)gtk.resolve(#name)
        DEC_FUNC(gtk_menu_new);
        DEC_FUNC(gtk_menu_item_new_with_label);
        DEC_FUNC(gtk_menu_shell_append);
        DEC_FUNC(gtk_widget_show);
#undef DEC_FUNC
        d->gmenu = gtk_menu_new();
        auto quit = gtk_menu_item_new_with_label(tr("Quit").toLocal8Bit());
        auto show = gtk_menu_item_new_with_label(tr("Show").toLocal8Bit());
        gtk_menu_shell_append(GTK_MENU_SHELL(d->gmenu), show);
        gtk_menu_shell_append(GTK_MENU_SHELL(d->gmenu), quit);
        gtk_widget_show(show);
        gtk_widget_show(quit);
        g_signal_connect(show, "activate", G_CALLBACK(onShow), this);
        g_signal_connect(quit, "activate", G_CALLBACK(onQuit), this);

        QLibrary ai(_L("libappindicator"), 1);
#define DEC_FUNC(name) auto name = (f_##name)ai.resolve(#name)
        DEC_FUNC(app_indicator_new);
        DEC_FUNC(app_indicator_set_menu);
#undef DEC_FUNC
        app_indicator_set_status = (f_app_indicator_set_status)ai.resolve("app_indicator_set_status");
        d->indicator = app_indicator_new("net.xylosper.CMPlayer.AppIndicator", "cmplayer", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
        app_indicator_set_menu(d->indicator, GTK_MENU(d->gmenu));
    }
	if (!d->unity) {
		d->tray = new QSystemTrayIcon(icon, parent);
		connect(d->tray, &QSystemTrayIcon::activated, [this] (QSystemTrayIcon::ActivationReason r) {
			emit activated(ActivationReason(r));
		});
	}
}

TrayIcon::~TrayIcon() {
	delete d->tray;
	delete d;
}

void TrayIcon::setVisible(bool visible) {
    if (d->unity && d->indicator) {
        app_indicator_set_status(d->indicator, visible ? APP_INDICATOR_STATUS_ACTIVE : APP_INDICATOR_STATUS_PASSIVE);
    } else if (d->tray) {
        d->tray->setVisible(visible);
	}
}

void TrayIcon::onShow(void *menu, void *arg) {
	Q_UNUSED(menu);
	auto p = static_cast<TrayIcon*>(arg);
    emit p->activated(Show);
}

void TrayIcon::onQuit(void *menu, void *arg) {
	Q_UNUSED(menu);
	auto p = static_cast<TrayIcon*>(arg);
	emit p->activated(Quit);
}
