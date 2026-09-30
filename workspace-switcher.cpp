#include <gtk/gtk.h>
#include <gtk-layer-shell.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <filesystem>
#include <vector>
#include <sstream>
#include <memory>
#include <unistd.h>
#include <chrono>
#include <algorithm>
#include <thread>
#include <future>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <regex>
#include <array>

struct WallpaperPalette {
    std::string accent;
    std::string accent_soft;
    std::string accent_pale;
    std::string accent_deep;
    std::string ink;
    std::string text;
};

static WallpaperPalette parse_wallpaper_colors() {
    WallpaperPalette p;
    p.accent       = "#c780fa";
    p.accent_soft  = "#e0b0ff";
    p.accent_pale  = "#f0d0ff";
    p.accent_deep  = "#7c3aed";
    p.ink          = "#0d0d0d";
    p.text         = "#ffffff";

    std::string home = getenv("HOME") ? getenv("HOME") : "";
    std::string path = home + "/.config/waybar/wallpaper-colors.css";
    std::ifstream f(path);
    if (!f.is_open()) return p;

    std::regex re("@define-color\\s+(\\S+)\\s+(#[0-9a-fA-F]{6,8})");
    std::string line;
    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            std::string key = m[1];
            std::string val = m[2];
            if      (key == "wallpaper_accent")      p.accent      = val;
            else if (key == "wallpaper_accent_soft")  p.accent_soft = val;
            else if (key == "wallpaper_accent_pale")  p.accent_pale = val;
            else if (key == "wallpaper_accent_deep")  p.accent_deep = val;
            else if (key == "wallpaper_ink")          p.ink         = val;
            else if (key == "wallpaper_text")         p.text        = val;
        }
    }
    return p;
}

static std::string hex_to_rgba(const std::string& hex, double alpha = 1.0) {
    if (hex.size() < 7) return "rgba(255,255,255," + std::to_string(alpha) + ")";
    int r = std::stoi(hex.substr(1, 2), nullptr, 16);
    int g = std::stoi(hex.substr(3, 2), nullptr, 16);
    int b = std::stoi(hex.substr(5, 2), nullptr, 16);
    std::ostringstream os;
    os << "rgba(" << r << "," << g << "," << b << "," << alpha << ")";
    return os.str();
}

static std::array<std::string, 14> build_glow_colors(const WallpaperPalette& p) {
    std::array<std::string, 14> c;
    c[0]  = "";
    c[1]  = p.accent_pale;
    c[2]  = p.accent_soft;
    c[3]  = p.accent;
    c[4]  = p.accent_deep;
    c[5]  = p.accent_soft;
    c[6]  = p.accent_pale;
    c[7]  = p.accent;
    c[8]  = p.accent_deep;
    c[9]  = p.accent_soft;
    c[10] = p.accent_pale;
    c[11] = p.accent;
    c[12] = p.accent_deep;
    c[13] = p.text;
    return c;
}

static std::string build_glow_css(int ws, const std::string& color) {
    std::string rgba06 = hex_to_rgba(color, 0.6);
    std::string rgba04 = hex_to_rgba(color, 0.4);
    std::ostringstream css;
    css << ".workspace-" << ws << ":hover {"
        << "color:" << color << ";"
        << "box-shadow:"
        << "inset 0 0 15px " << rgba06 << ","
        << "inset 0 0 30px " << rgba04 << ","
        << "0 0 20px " << color << ","
        << "0 0 40px " << color << ","
        << "0 0 60px " << color << ";"
        << "animation:pulse-glow 1.5s infinite ease-in-out;"
        << "}";
    return css.str();
}

class WorkspaceSwitcher {
private:
    GtkWidget* window;
    GtkWidget* fixed;
    GtkWidget* tooltip_window  = nullptr;
    GtkWidget* tooltip_label   = nullptr;
    GtkWidget* tooltip_image   = nullptr;

    std::unordered_map<int, GdkPixbuf*>              workspace_icon_cache;
    std::unordered_map<int, std::vector<GdkPixbuf*>> app_icon_cache;
    std::unordered_map<std::string, GdkPixbuf*>      theme_icon_cache;
    std::unordered_map<int, std::vector<std::string>> workspace_apps_cache;
    std::unordered_map<int, std::vector<GtkWidget*>> app_icon_widgets;
    std::unordered_map<int, std::vector<std::string>> workspace_app_classes;
    std::unordered_map<int, GtkWidget*>              workspace_buttons;
    std::mutex cache_mutex;

    bool  fade_in_complete       = false;
    guint fade_timeout_id        = 0;
    guint app_icon_loader_id     = 0;
    guint workspace_icon_loader_id = 0;

    int screen_width, screen_height;
    int center_x, center_y;
    int button_size, icon_size, app_icon_size, special_button_size;
    std::string workspace_icon_path;
    WallpaperPalette palette;
    std::array<std::string, 14> glow_colors;

    static gboolean on_button_enter_static(GtkWidget*, GdkEventCrossing*, gpointer);
    static gboolean on_button_leave_static(GtkWidget*, GdkEventCrossing*, gpointer);
    static void     on_workspace_click_static(GtkWidget*, gpointer);
    static gboolean on_key_press_static(GtkWidget*, GdkEventKey*, gpointer);
    static void     on_destroy_static(GtkWidget*, gpointer);
    static gboolean fade_in_timeout_static(gpointer);
    static gboolean load_app_icons_async_static(gpointer);
    static gboolean load_workspace_icons_async_static(gpointer);

    void calculate_dimensions() {
        GdkScreen* screen = gdk_screen_get_default();
        screen_width  = gdk_screen_get_width(screen);
        screen_height = gdk_screen_get_height(screen);
        center_x = screen_width  / 2;
        center_y = screen_height / 2;
        int min_dim   = std::min(screen_width, screen_height);
        button_size   = std::max(100, static_cast<int>(min_dim * 0.075));
        icon_size     = std::max(50,  static_cast<int>(button_size * 0.83));
        app_icon_size = std::max(16,  static_cast<int>(button_size * 0.17));
        special_button_size = button_size * 2;
    }

    std::string determine_workspace_icon_path() {
        std::string home = getenv("HOME") ? getenv("HOME") : "";
        std::string base = home + "/.config/Light/assets/workspace/";
        std::string light_path = home + "/.config/hypr/Light.txt";

        if (!std::filesystem::exists(light_path)) return base;

        std::ifstream f(light_path);
        if (!f.is_open()) return base;
        std::string content, line;
        while (std::getline(f, line)) content += line;

        if (content.find("color") != std::string::npos)  return base + "COLOR/";
        if (content.find("cyrene") != std::string::npos) return base + "AMPH/";
        return base;
    }

    // Death Note layout: ws13 top-center (eye of god), ws1-6 left column descending,
    // ws7-12 right column descending — like pages falling from a Death Note spine.
    // Columns are inset from screen edges, staggered vertically for a dramatic asymmetry.
    struct WsPos { int x; int y; };

    WsPos workspace_position(int ws) {
        int col_top     = static_cast<int>(screen_height * 0.12);
        int col_spacing = static_cast<int>((screen_height * 0.82) / 6.5);
        int left_x  = static_cast<int>(screen_width * 0.22);
        int right_x = static_cast<int>(screen_width * 0.78);
        int stagger = button_size / 3;

        if (ws == 13) {
            return { center_x, static_cast<int>(screen_height * 0.07) };
        }
        if (ws >= 1 && ws <= 6) {
            int row = ws - 1;
            int x   = left_x + (row % 2 == 0 ? 0 : stagger);
            int y   = col_top + row * col_spacing;
            return { x, y };
        }
        if (ws >= 7 && ws <= 12) {
            int row = ws - 7;
            int x   = right_x + (row % 2 == 0 ? 0 : -stagger);
            int y   = col_top + row * col_spacing;
            return { x, y };
        }
        return { center_x, center_y };
    }

public:
    WorkspaceSwitcher() {
        palette     = parse_wallpaper_colors();
        glow_colors = build_glow_colors(palette);
        calculate_dimensions();
        workspace_icon_path = determine_workspace_icon_path();
        create_window();
        setup_layer_shell();
        create_workspace_buttons_minimal();
        apply_minimal_css();
        connect_signals();
        gtk_widget_show_all(window);
        gtk_widget_grab_focus(window);
        start_fade_in_animation();
        workspace_icon_loader_id = g_idle_add_full(G_PRIORITY_HIGH,   load_workspace_icons_async_static, this, nullptr);
        app_icon_loader_id       = g_idle_add_full(G_PRIORITY_LOW,    load_app_icons_async_static,       this, nullptr);
        g_idle_add_full(G_PRIORITY_LOW, [](gpointer ud) -> gboolean {
            auto* self = static_cast<WorkspaceSwitcher*>(ud);
            self->create_tooltip();
            self->apply_full_css();
            return FALSE;
        }, this, nullptr);
    }

    ~WorkspaceSwitcher() {
        cleanup_caches();
        if (fade_timeout_id        > 0) g_source_remove(fade_timeout_id);
        if (app_icon_loader_id     > 0) g_source_remove(app_icon_loader_id);
        if (workspace_icon_loader_id > 0) g_source_remove(workspace_icon_loader_id);
    }

    void cleanup_caches() {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (auto& p : workspace_icon_cache) if (p.second) g_object_unref(p.second);
        workspace_icon_cache.clear();
        for (auto& p : app_icon_cache)
            for (auto* px : p.second) if (px) g_object_unref(px);
        app_icon_cache.clear();
        for (auto& p : theme_icon_cache) if (p.second) g_object_unref(p.second);
        theme_icon_cache.clear();
    }

    gboolean load_workspace_icons_async() {
        static int ws = 1;
        if (ws > 13) { workspace_icon_loader_id = 0; return FALSE; }
        load_workspace_icon(ws++);
        return TRUE;
    }

    void load_workspace_icon(int id) {
        std::string path = workspace_icon_path + std::to_string(id) + ".png";
        if (!std::filesystem::exists(path)) return;
        GError* err = nullptr;
        int sz = (id == 13) ? static_cast<int>(icon_size * 1.8) : icon_size;
        GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_size(path.c_str(), sz, sz, &err);
        if (err) { g_error_free(err); return; }
        if (!pb) return;
        auto it = workspace_buttons.find(id);
        if (it != workspace_buttons.end()) {
            GList* ch = gtk_container_get_children(GTK_CONTAINER(it->second));
            if (ch) { gtk_widget_destroy(GTK_WIDGET(ch->data)); g_list_free(ch); }
            GtkWidget* img = gtk_image_new_from_pixbuf(pb);
            gtk_style_context_add_class(gtk_widget_get_style_context(img), "workspace-icon");
            gtk_container_add(GTK_CONTAINER(it->second), img);
            gtk_widget_show(img);
        }
        std::lock_guard<std::mutex> lock(cache_mutex);
        workspace_icon_cache[id] = pb;
    }

    gboolean load_app_icons_async() {
        static int ws = 1;
        if (ws > 13) { app_icon_loader_id = 0; return FALSE; }
        load_workspace_app_icons(ws++);
        return TRUE;
    }

    void load_workspace_app_icons(int ws) {
        auto app_classes = get_workspace_app_classes(ws);
        if (app_classes.empty()) return;
        auto pos = workspace_position(ws);
        int base_x = pos.x, base_y = pos.y;
        int max_icons   = std::min(4, (int)app_classes.size());
        int icon_spacing = std::max(20, app_icon_size + 5);
        int start_offset = -(max_icons - 1) * icon_spacing / 2;
        int bsz = (ws == 13) ? special_button_size : button_size;
        for (int j = 0; j < max_icons; j++) {
            GdkPixbuf* pb = get_app_icon(app_classes[j]);
            if (!pb) continue;
            int ix = base_x + start_offset + j * icon_spacing - app_icon_size / 2;
            int iy = base_y + bsz / 2 + 10;
            GtkWidget* img = gtk_image_new_from_pixbuf(pb);
            gtk_style_context_add_class(gtk_widget_get_style_context(img), "app-icon");
            gtk_fixed_put(GTK_FIXED(fixed), img, ix, iy);
            gtk_widget_show(img);
            std::lock_guard<std::mutex> lock(cache_mutex);
            app_icon_cache[ws].push_back(g_object_ref(pb));
            app_icon_widgets[ws].push_back(img);
            g_object_unref(pb);
        }
    }

    void start_fade_in_animation() {
        gtk_widget_set_opacity(window, 0.0);
        fade_timeout_id = g_timeout_add(8, fade_in_timeout_static, this);
    }

    gboolean fade_in_timeout() {
        static double opacity = 0.0;
        opacity += 0.12;
        if (opacity >= 1.0) {
            opacity = 1.0;
            fade_in_complete = true;
            fade_timeout_id  = 0;
            gtk_widget_queue_draw(window);
            return FALSE;
        }
        gtk_widget_set_opacity(window, opacity);
        return TRUE;
    }

    static std::string get_screenshot_path(int id) {
        return "/tmp/workspace_previews/workspace_" + std::to_string(id) + ".png";
    }

    GdkPixbuf* create_workspace_thumbnail_from_path(const std::string& p) {
        if (p.empty() || !std::filesystem::exists(p)) return nullptr;
        GError* err = nullptr;
        int w = std::max(200, screen_width / 6);
        int h = std::max(112, (int)(w * 9.0 / 16.0));
        GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_size(p.c_str(), w, h, &err);
        if (err) { g_error_free(err); return nullptr; }
        return pb;
    }

    std::vector<std::string> get_workspace_app_classes(int ws) {
        std::vector<std::string> v;
        std::string cmd = (ws == 13)
            ? "hyprctl clients -j 2>/dev/null | jq -r '.[] | select(.workspace.name == \"special:light\") | .class' 2>/dev/null"
            : "hyprctl clients -j 2>/dev/null | jq -r '.[] | select(.workspace.id == " + std::to_string(ws) + ") | .class' 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "r"); if (!p) return v;
        char buf[128];
        while (fgets(buf, sizeof(buf), p)) {
            std::string s = buf;
            if (!s.empty() && s.back() == '\n') s.pop_back();
            if (!s.empty()) v.push_back(s);
        }
        pclose(p);
        return v;
    }

    GdkPixbuf* get_app_icon(const std::string& cls) {
        if (cls.empty()) return nullptr;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto it = theme_icon_cache.find(cls);
            if (it != theme_icon_cache.end())
                return it->second ? g_object_ref(it->second) : nullptr;
        }
        GtkIconTheme* theme = gtk_icon_theme_get_default();
        std::string name = cls;
        if (!gtk_icon_theme_has_icon(theme, name.c_str()))
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        GError* err = nullptr;
        GdkPixbuf* pb = gtk_icon_theme_load_icon(theme, name.c_str(), app_icon_size, GTK_ICON_LOOKUP_FORCE_SIZE, &err);
        if (err) {
            g_error_free(err);
            for (auto& fb : std::vector<std::string>{"application-x-executable","application-default-icon","application","window","folder"}) {
                err = nullptr;
                pb = gtk_icon_theme_load_icon(theme, fb.c_str(), app_icon_size, GTK_ICON_LOOKUP_FORCE_SIZE, &err);
                if (pb && !err) break;
                if (err) g_error_free(err);
            }
        }
        std::lock_guard<std::mutex> lock(cache_mutex);
        theme_icon_cache[cls] = pb ? g_object_ref(pb) : nullptr;
        return pb;
    }

    std::vector<std::string> get_workspace_apps(int ws) {
        std::vector<std::string> v;
        std::string cmd = (ws == 13)
            ? "hyprctl clients -j 2>/dev/null | jq -r '.[] | select(.workspace.name == \"special:light\") | .title' 2>/dev/null"
            : "hyprctl clients -j 2>/dev/null | jq -r '.[] | select(.workspace.id == " + std::to_string(ws) + ") | .title' 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "r"); if (!p) return v;
        char buf[256];
        while (fgets(buf, sizeof(buf), p)) {
            std::string s = buf;
            if (!s.empty() && s.back() == '\n') s.pop_back();
            if (!s.empty()) v.push_back(s);
        }
        pclose(p);
        return v;
    }

    void create_tooltip() {
        tooltip_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_decorated(GTK_WINDOW(tooltip_window), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(tooltip_window), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(tooltip_window), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(tooltip_window), TRUE);
        gtk_window_set_type_hint(GTK_WINDOW(tooltip_window), GDK_WINDOW_TYPE_HINT_TOOLTIP);
        gtk_layer_init_for_window(GTK_WINDOW(tooltip_window));
        gtk_layer_set_layer(GTK_WINDOW(tooltip_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(tooltip_window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_start(vbox, 10);
        gtk_widget_set_margin_end(vbox, 10);
        gtk_widget_set_margin_top(vbox, 10);
        gtk_widget_set_margin_bottom(vbox, 10);
        tooltip_image = gtk_image_new();
        gtk_box_pack_start(GTK_BOX(vbox), tooltip_image, FALSE, FALSE, 0);
        tooltip_label = gtk_label_new("");
        gtk_label_set_justify(GTK_LABEL(tooltip_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(vbox), tooltip_label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(tooltip_window), vbox);
        gtk_style_context_add_class(gtk_widget_get_style_context(tooltip_window), "tooltip-window");
        gtk_widget_add_events(tooltip_window, GDK_KEY_PRESS_MASK);
        g_signal_connect(tooltip_window, "key-press-event", G_CALLBACK(on_key_press_static), this);
    }

    void create_window() {
        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), "Workspace Switcher");
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
        gtk_window_set_default_size(GTK_WINDOW(window), screen_width, screen_height);
        gtk_window_set_accept_focus(GTK_WINDOW(window), TRUE);
        gtk_window_set_focus_on_map(GTK_WINDOW(window), TRUE);
        gtk_widget_add_events(window, GDK_KEY_PRESS_MASK);
        gtk_widget_set_app_paintable(window, TRUE);
        fixed = gtk_fixed_new();
        gtk_container_add(GTK_CONTAINER(window), fixed);
    }

    void setup_layer_shell() {
        gtk_layer_init_for_window(GTK_WINDOW(window));
        gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP,    TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT,   TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT,  TRUE);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    }

    void create_workspace_buttons_minimal() {
        for (int i = 1; i <= 13; i++) {
            int bsz = (i == 13) ? special_button_size : button_size;
            GtkWidget* btn = gtk_button_new_with_label(std::to_string(i).c_str());
            gtk_widget_set_size_request(btn, bsz, bsz);
            gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
            auto* ctx = gtk_widget_get_style_context(btn);
            gtk_style_context_add_class(ctx, "workspace-button");
            gtk_style_context_add_class(ctx, ("workspace-" + std::to_string(i)).c_str());
            g_signal_connect(btn, "enter-notify-event", G_CALLBACK(on_button_enter_static), this);
            g_signal_connect(btn, "leave-notify-event", G_CALLBACK(on_button_leave_static), this);
            gtk_widget_set_events(btn, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
            g_object_set_data(G_OBJECT(btn), "workspace", GINT_TO_POINTER(i));
            g_signal_connect(btn, "clicked", G_CALLBACK(on_workspace_click_static), this);
            auto pos = workspace_position(i);
            gtk_fixed_put(GTK_FIXED(fixed), btn, pos.x - bsz / 2, pos.y - bsz / 2);
            workspace_buttons[i] = btn;
        }
    }

    void show_tooltip(int ws, gint tx, gint ty) {
        if (!tooltip_window) return;
        auto apps = get_workspace_apps(ws);
        if (!apps.empty()) {
            GdkPixbuf* thumb = create_workspace_thumbnail_from_path(get_screenshot_path(ws));
            if (thumb) {
                gtk_image_set_from_pixbuf(GTK_IMAGE(tooltip_image), thumb);
                gtk_widget_show(tooltip_image);
                g_object_unref(thumb);
            } else {
                gtk_image_clear(GTK_IMAGE(tooltip_image));
                gtk_widget_hide(tooltip_image);
            }
        } else {
            gtk_image_clear(GTK_IMAGE(tooltip_image));
            gtk_widget_hide(tooltip_image);
        }
        std::string text = (ws == 13) ? "Special Workspace (Light)" : "Workspace " + std::to_string(ws);
        text += apps.empty() ? "\nNothing" : " (" + std::to_string(apps.size()) + " apps):";
        for (auto& a : apps) text += "\n• " + a;
        gtk_label_set_text(GTK_LABEL(tooltip_label), text.c_str());
        gtk_widget_show_all(tooltip_window);
        GtkRequisition tsz;
        gtk_widget_get_preferred_size(tooltip_window, &tsz, nullptr);
        int x = tx - tsz.width / 2;
        int y = ty - tsz.height - 20;
        x = std::max(10, std::min(x, screen_width  - tsz.width  - 10));
        y = std::max(10, std::min(y, screen_height - tsz.height - 10));
        gtk_layer_set_margin(GTK_WINDOW(tooltip_window), GTK_LAYER_SHELL_EDGE_LEFT, x);
        gtk_layer_set_margin(GTK_WINDOW(tooltip_window), GTK_LAYER_SHELL_EDGE_TOP,  y);
    }

    void hide_tooltip() {
        if (tooltip_window) gtk_widget_hide(tooltip_window);
    }

    bool is_currently_on_special_workspace() {
        FILE* p = popen("hyprctl -j activewindow 2>/dev/null | jq -r '.workspace.id' 2>/dev/null", "r");
        if (!p) return false;
        char buf[128]; bool result = false;
        if (fgets(buf, sizeof(buf), p)) {
            std::string s = buf;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            try { result = (std::stoi(s) < 0); } catch (...) {}
        }
        pclose(p);
        return result;
    }

    void switch_workspace(int ws) {
        bool on_special = is_currently_on_special_workspace();
        if (ws == 13) {
            system("hyprctl dispatch \"hl.dsp.workspace.toggle_special('light')\"");
        } else {
            if (on_special)
                system("hyprctl dispatch \"hl.dsp.workspace.toggle_special('light')\"");
            std::string cmd = "hyprctl dispatch \"hl.dsp.focus({workspace = '" + std::to_string(ws) + "'})\"";
            system(cmd.c_str());
        }
    }

    gboolean on_key_press(GdkEventKey* e) {
        switch (e->keyval) {
            case GDK_KEY_Escape:    gtk_main_quit(); return TRUE;
            case GDK_KEY_1:         switch_workspace( 1); gtk_main_quit(); return TRUE;
            case GDK_KEY_2:         switch_workspace( 2); gtk_main_quit(); return TRUE;
            case GDK_KEY_3:         switch_workspace( 3); gtk_main_quit(); return TRUE;
            case GDK_KEY_4:         switch_workspace( 4); gtk_main_quit(); return TRUE;
            case GDK_KEY_5:         switch_workspace( 5); gtk_main_quit(); return TRUE;
            case GDK_KEY_6:         switch_workspace( 6); gtk_main_quit(); return TRUE;
            case GDK_KEY_7:         switch_workspace( 7); gtk_main_quit(); return TRUE;
            case GDK_KEY_8:         switch_workspace( 8); gtk_main_quit(); return TRUE;
            case GDK_KEY_9:         switch_workspace( 9); gtk_main_quit(); return TRUE;
            case GDK_KEY_0:         switch_workspace(10); gtk_main_quit(); return TRUE;
            case GDK_KEY_minus:     switch_workspace(11); gtk_main_quit(); return TRUE;
            case GDK_KEY_equal:     switch_workspace(12); gtk_main_quit(); return TRUE;
            case GDK_KEY_BackSpace: switch_workspace(13); gtk_main_quit(); return TRUE;
            default: return FALSE;
        }
    }

    void connect_signals() {
        g_signal_connect(window, "destroy",        G_CALLBACK(on_destroy_static),   this);
        g_signal_connect(window, "key-press-event",G_CALLBACK(on_key_press_static), this);
        gtk_widget_set_can_focus(window, TRUE);
        gtk_widget_grab_focus(window);
    }

    void apply_minimal_css() {
        const char* css = R"(
            window { background: transparent; }
            .workspace-button {
                background: transparent;
                border: none;
                border-radius: 50%;
                color: white;
                font-weight: bold;
                transition: transform 0.1s ease;
            }
            .workspace-button:hover { transform: scale(1.1); }
        )";
        GtkCssProvider* pv = gtk_css_provider_new();
        gtk_css_provider_load_from_data(pv, css, -1, nullptr);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(pv), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(pv);
    }

    void apply_full_css() {
        // -- build dynamic per-workspace glow rules from wallpaper palette --
        std::ostringstream glow_rules;
        for (int i = 1; i <= 13; i++)
            glow_rules << build_glow_css(i, glow_colors[i]);

        // ws-13 gets stronger glow + faster pulse
        glow_rules << ".workspace-13:hover {"
                   << "color:" << palette.text << ";"
                   << "box-shadow:"
                   << "inset 0 0 20px " << hex_to_rgba(palette.text, 0.8) << ","
                   << "inset 0 0 40px " << hex_to_rgba(palette.accent, 0.5) << ","
                   << "0 0 30px " << palette.text << ","
                   << "0 0 60px " << palette.accent << ","
                   << "0 0 90px " << palette.accent_deep << ";"
                   << "animation:pulse-glow 1.0s infinite ease-in-out;}";

        // vertical connector lines between columns — Death Note spine feel
        // drawn as thin box-shadows on column buttons
        std::string left_connector  = hex_to_rgba(palette.accent,      0.25);
        std::string right_connector = hex_to_rgba(palette.accent_soft,  0.25);

        std::ostringstream connector_rules;
        for (int i = 1; i <= 6; i++)
            connector_rules << ".workspace-" << i
                            << " { border-left: 1px solid " << left_connector << "; }";
        for (int i = 7; i <= 12; i++)
            connector_rules << ".workspace-" << i
                            << " { border-right: 1px solid " << right_connector << "; }";

        std::string ink_rgba    = hex_to_rgba(palette.ink,          0.70);
        std::string accent_rgba = hex_to_rgba(palette.accent,       0.18);
        std::string deep_rgba   = hex_to_rgba(palette.accent_deep,  0.30);

        std::ostringstream full_css;
        full_css << R"(
            @keyframes pulse-glow {
                0%   { box-shadow: inset 0 0 10px currentColor, inset 0 0 20px currentColor,
                                   0 0 15px currentColor, 0 0 30px currentColor, 0 0 45px currentColor;
                       transform: scale(1.0); }
                50%  { box-shadow: inset 0 0 20px currentColor, inset 0 0 40px currentColor,
                                   0 0 25px currentColor, 0 0 50px currentColor, 0 0 75px currentColor;
                       transform: scale(1.05); }
                100% { box-shadow: inset 0 0 10px currentColor, inset 0 0 20px currentColor,
                                   0 0 15px currentColor, 0 0 30px currentColor, 0 0 45px currentColor;
                       transform: scale(1.0); }
            }
            @keyframes fade-in {
                from { opacity: 0; transform: scale(0.95); }
                to   { opacity: 1; transform: scale(1.0); }
            }
            window { background: transparent; }
            .workspace-icon { animation: fade-in 0.3s ease-out; }
            .workspace-button {
                background: transparent;
                border: none;
                border-radius: 50%;
                color: white;
                font-weight: bold;
                transition: all 0.1s cubic-bezier(0.25, 0.46, 0.45, 0.94);
            }
            .workspace-button:hover {
                background: transparent;
                transform: scale(1.1);
            }
            .workspace-button:active {
                background: )" << accent_rgba << R"(;
                transform: scale(0.95);
                transition: all 0.05s ease;
            }
            .app-icon {
                background: transparent;
                border-radius: 10px;
                opacity: 0.8;
                transition: all 0.1s cubic-bezier(0.25, 0.46, 0.45, 0.94);
            }
            .app-icon:hover { opacity: 1.0; transform: scale(1.1); }
            .tooltip-window {
                background: )" << ink_rgba << R"(;
                border: 1px solid )" << deep_rgba << R"(;
                border-radius: 16px;
                color: )" << palette.text << R"(;
                font-size: 14px;
                text-shadow: 1px 1px 3px rgba(0,0,0,0.8);
            }
        )";
        full_css << glow_rules.str();
        full_css << connector_rules.str();

        GtkCssProvider* pv = gtk_css_provider_new();
        std::string css_str = full_css.str();
        gtk_css_provider_load_from_data(pv, css_str.c_str(), -1, nullptr);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(pv), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        g_object_unref(pv);
    }

    void run() { gtk_main(); }
};

gboolean WorkspaceSwitcher::load_workspace_icons_async_static(gpointer u) {
    return static_cast<WorkspaceSwitcher*>(u)->load_workspace_icons_async();
}
gboolean WorkspaceSwitcher::load_app_icons_async_static(gpointer u) {
    return static_cast<WorkspaceSwitcher*>(u)->load_app_icons_async();
}
gboolean WorkspaceSwitcher::fade_in_timeout_static(gpointer u) {
    return static_cast<WorkspaceSwitcher*>(u)->fade_in_timeout();
}

gboolean WorkspaceSwitcher::on_button_enter_static(GtkWidget* btn, GdkEventCrossing* ev, gpointer u) {
    (void)ev;
    auto* self = static_cast<WorkspaceSwitcher*>(u);
    if (!self->fade_in_complete) return FALSE;
    int ws = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "workspace"));
    auto pos = self->workspace_position(ws);
    int bsz = (ws == 13) ? self->special_button_size : self->button_size;
    self->show_tooltip(ws, pos.x, pos.y - bsz / 2 - 5);
    return FALSE;
}

gboolean WorkspaceSwitcher::on_button_leave_static(GtkWidget*, GdkEventCrossing*, gpointer u) {
    static_cast<WorkspaceSwitcher*>(u)->hide_tooltip();
    return FALSE;
}

void WorkspaceSwitcher::on_workspace_click_static(GtkWidget* btn, gpointer u) {
    auto* self = static_cast<WorkspaceSwitcher*>(u);
    int ws = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "workspace"));
    self->switch_workspace(ws);
    gtk_main_quit();
}

gboolean WorkspaceSwitcher::on_key_press_static(GtkWidget*, GdkEventKey* e, gpointer u) {
    return static_cast<WorkspaceSwitcher*>(u)->on_key_press(e);
}

void WorkspaceSwitcher::on_destroy_static(GtkWidget*, gpointer u) {
    auto* self = static_cast<WorkspaceSwitcher*>(u);
    self->cleanup_caches();
    gtk_main_quit();
}

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);
    g_object_set(gtk_settings_get_default(),
                 "gtk-enable-animations", TRUE,
                 "gtk-animation-duration", 5,
                 nullptr);
    WorkspaceSwitcher app;
    app.run();
    return 0;
}
