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

class WorkspaceSwitcher {
private:
    GtkWidget* window;
    GtkWidget* fixed;
    GtkWidget* tooltip_window;
    GtkWidget* tooltip_label;
    GtkWidget* tooltip_image;
    // Performance optimization: Cache pixbufs and app data
    std::unordered_map<int, GdkPixbuf*> workspace_icon_cache;
    std::unordered_map<int, std::vector<GdkPixbuf*>> app_icon_cache;
    std::unordered_map<std::string, GdkPixbuf*> theme_icon_cache;
    std::unordered_map<int, std::vector<std::string>> workspace_apps_cache;
    std::unordered_map<int, std::vector<GtkWidget*>> app_icon_widgets;
    std::unordered_map<int, std::vector<std::string>> workspace_app_classes;
    std::unordered_map<int, GtkWidget*> workspace_buttons; // Track buttons for icon updates
    std::mutex cache_mutex;
    // Animation and loading state
    bool fade_in_complete = false;
    guint fade_timeout_id = 0;
    guint app_icon_loader_id = 0;
    guint workspace_icon_loader_id = 0; // For async workspace icon loading
    // Dynamic screen dimensions
    int screen_width;
    int screen_height;
    int center_x;
    int center_y;
    int radius;
    int button_size;
    int icon_size;
    int app_icon_size;
    int special_button_size; // Size for workspace 13
    std::string workspace_icon_path; // Theme-specific workspace icon path

    // Static callbacks
    static gboolean on_button_enter_static(GtkWidget* button, GdkEventCrossing* event, gpointer user_data);
    static gboolean on_button_leave_static(GtkWidget* button, GdkEventCrossing* event, gpointer user_data);
    static void     on_workspace_click_static(GtkWidget* button, gpointer user_data);
    static gboolean on_key_press_static(GtkWidget* widget, GdkEventKey* event, gpointer user_data);
    static void     on_destroy_static(GtkWidget* widget, gpointer user_data);
    static gboolean fade_in_timeout_static(gpointer user_data);
    static gboolean load_app_icons_async_static(gpointer user_data);
    static gboolean load_workspace_icons_async_static(gpointer user_data);

    void calculate_dimensions() {
        GdkScreen* screen = gdk_screen_get_default();
        screen_width = gdk_screen_get_width(screen);
        screen_height = gdk_screen_get_height(screen);
        center_x = screen_width / 2;
        center_y = screen_height / 2;
        // Scale radius based on screen size, with minimum for small screens
        int min_dimension = std::min(screen_width, screen_height);
        radius = std::max(200, static_cast<int>(min_dimension * 0.40));
        // Scale button and icon sizes based on screen size
        button_size = std::max(120, static_cast<int>(min_dimension * 0.08));
        icon_size = std::max(50, static_cast<int>(button_size * 0.83));
        app_icon_size = std::max(16, static_cast<int>(button_size * 0.17));
        // --- CHANGE: Make workspace 13 button 2x the size ---
        // special_button_size = static_cast<int>(button_size * 1.5); // Old size
        special_button_size = button_size * 2; // New size: 2x regular button size
        // --- END CHANGE ---
    }

    std::string determine_workspace_icon_path() {
        std::string home_dir = getenv("HOME") ? getenv("HOME") : "";
        std::string light_txt_path = home_dir + "/.config/hypr/Light.txt";
        
        // Default to Theme 1 (current/ely theme)
        std::string default_path = home_dir + "/.config/Light/assets/workspace/";
        
        // Check if Light.txt exists
        if (!std::filesystem::exists(light_txt_path)) {
            return default_path;
        }
        
        // Read the file content
        std::ifstream file(light_txt_path);
        if (!file.is_open()) {
            return default_path;
        }
        
        std::string content;
        std::string line;
        while (std::getline(file, line)) {
            content += line;
        }
        file.close();
        
        // Check for "cyrene" theme
        if (content.find("cyrene") != std::string::npos) {
            // Theme 2: cyrene
            return home_dir + "/.config/Light/assets/workspace/AMPH/";
        }
        
        // Check for "ely" theme or default to Theme 1
        if (content.find("ely") != std::string::npos) {
            // Theme 1: ely (current theme)
            return default_path;
        }
        
        // If file exists but doesn't contain either keyword, default to Theme 1
        return default_path;
    }

public:
    WorkspaceSwitcher() {
        // Minimal startup - just show the window ASAP
        calculate_dimensions();
        // Determine workspace icon path based on theme
        workspace_icon_path = determine_workspace_icon_path();
        create_window();
        setup_layer_shell();
        // Create buttons immediately but without icons (fastest)
        create_workspace_buttons_minimal();
        // Apply minimal CSS first
        apply_minimal_css();
        connect_signals();
        // Show UI immediately - this is the key to fast startup
        gtk_widget_show_all(window);
        gtk_widget_grab_focus(window);
        // Start everything else asynchronously after UI is visible
        start_fade_in_animation();
        // Defer all heavy operations with different priorities
        workspace_icon_loader_id = g_idle_add_full(G_PRIORITY_HIGH, load_workspace_icons_async_static, this, nullptr);
        app_icon_loader_id = g_idle_add_full(G_PRIORITY_LOW, load_app_icons_async_static, this, nullptr);
        // Defer tooltip creation and full CSS loading
        g_idle_add_full(G_PRIORITY_LOW, [](gpointer user_data) -> gboolean {
            WorkspaceSwitcher* self = static_cast<WorkspaceSwitcher*>(user_data);
            self->create_tooltip();
            self->apply_full_css();
            return FALSE; // Run once
        }, this, nullptr);
    }

    ~WorkspaceSwitcher() {
        cleanup_caches();
        if (fade_timeout_id > 0) {
            g_source_remove(fade_timeout_id);
        }
        if (app_icon_loader_id > 0) {
            g_source_remove(app_icon_loader_id);
        }
        if (workspace_icon_loader_id > 0) {
            g_source_remove(workspace_icon_loader_id);
        }
    }

    void cleanup_caches() {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (auto& pair : workspace_icon_cache) {
            if (pair.second) g_object_unref(pair.second);
        }
        workspace_icon_cache.clear();
        for (auto& pair : app_icon_cache) {
            for (auto* pixbuf : pair.second) {
                if (pixbuf) g_object_unref(pixbuf);
            }
        }
        app_icon_cache.clear();
        for (auto& pair : theme_icon_cache) {
            if (pair.second) g_object_unref(pair.second);
        }
        theme_icon_cache.clear();
    }

    // Async workspace icon loading
    gboolean load_workspace_icons_async() {
        static int current_workspace = 1;
        if (current_workspace > 13) {
            workspace_icon_loader_id = 0;
            return FALSE; // Stop the idle callback
        }
        // Load one workspace icon at a time
        load_workspace_icon(current_workspace);
        current_workspace++;
        return TRUE; // Continue for next workspace
    }

    void load_workspace_icon(int workspace_id) {
        std::string image_path = workspace_icon_path + std::to_string(workspace_id) + ".png";
        if (!std::filesystem::exists(image_path)) {
            return; // Skip if file doesn't exist
        }
        GError* error = nullptr;
        int current_icon_size;
        // --- CHANGE: Adjust icon size for workspace 13 to better fit the larger button ---
        if (workspace_id == 13) {
            // current_icon_size = static_cast<int>(icon_size * 1.5); // Old size
            current_icon_size = static_cast<int>(icon_size * 1.8); // New size: 1.8x base icon size
        } else {
            current_icon_size = icon_size;
        }
        // --- END CHANGE ---
        GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_size(image_path.c_str(), current_icon_size, current_icon_size, &error);
        if (error) {
            g_error_free(error);
            return;
        }
        if (pixbuf) {
            // Update the button with the icon
            auto button_it = workspace_buttons.find(workspace_id);
            if (button_it != workspace_buttons.end()) {
                GtkWidget* button = button_it->second;
                // Remove existing label if any
                GList* children = gtk_container_get_children(GTK_CONTAINER(button));
                if (children) {
                    gtk_widget_destroy(GTK_WIDGET(children->data));
                    g_list_free(children);
                }
                // Add image
                GtkWidget* image = gtk_image_new_from_pixbuf(pixbuf);
                GtkStyleContext* img_context = gtk_widget_get_style_context(image);
                gtk_style_context_add_class(img_context, "workspace-icon");
                gtk_container_add(GTK_CONTAINER(button), image);
                gtk_widget_show(image);
            }
            // Cache the result
            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                workspace_icon_cache[workspace_id] = pixbuf;
            }
        }
    }

    // Async app icon loading to avoid blocking startup
    gboolean load_app_icons_async() {
        static int current_workspace = 1;
        if (current_workspace > 13) {
            app_icon_loader_id = 0;
            return FALSE; // Stop the idle callback
        }
        // Load icons for one workspace at a time to spread the work
        load_workspace_app_icons(current_workspace);
        current_workspace++;
        return TRUE; // Continue for next workspace
    }

    void load_workspace_app_icons(int workspace_id) {
        std::vector<std::string> app_classes = get_workspace_app_classes(workspace_id);
        if (app_classes.empty()) {
            return;
        }
        std::vector<GdkPixbuf*> workspace_app_icons;
        std::vector<GtkWidget*> workspace_icon_widgets;
        int base_x, base_y;
        if (workspace_id == 13) {
            // Special positioning for center workspace - place app icons below the button
            base_x = center_x;
            base_y = center_y + special_button_size/2 + 20;
        } else {
            // Calculate base position for regular workspaces
            double angle = (workspace_id - 1) * (2 * M_PI / 12) - (M_PI / 2);
            base_x = center_x + radius * cos(angle);
            base_y = center_y + radius * sin(angle);
        }
        // Limit to maximum 4 icons to avoid overcrowding
        int max_icons = std::min(4, static_cast<int>(app_classes.size()));
        int icon_spacing = std::max(20, app_icon_size + 5);
        int start_offset = -(max_icons - 1) * icon_spacing / 2;
        for (int j = 0; j < max_icons; j++) {
            GdkPixbuf* app_icon = get_app_icon(app_classes[j]);
            if (app_icon) {
                int icon_x = base_x + start_offset + (j * icon_spacing) - app_icon_size/2;
                int icon_y;
                if (workspace_id == 13) {
                    // For center workspace, place icons directly below
                    icon_y = base_y;
                } else {
                    // For regular workspaces, place icons below the button
                    icon_y = base_y + button_size/2 + 10;
                }
                GtkWidget* app_icon_image = gtk_image_new_from_pixbuf(app_icon);
                GtkStyleContext* icon_context = gtk_widget_get_style_context(app_icon_image);
                gtk_style_context_add_class(icon_context, "app-icon");
                gtk_fixed_put(GTK_FIXED(fixed), app_icon_image, icon_x, icon_y);
                gtk_widget_show(app_icon_image);
                workspace_app_icons.push_back(g_object_ref(app_icon));
                workspace_icon_widgets.push_back(app_icon_image);
                g_object_unref(app_icon);
            }
        }
        if (!workspace_app_icons.empty()) {
            std::lock_guard<std::mutex> lock(cache_mutex);
            app_icon_cache[workspace_id] = workspace_app_icons;
            app_icon_widgets[workspace_id] = workspace_icon_widgets;
            workspace_app_classes[workspace_id] = app_classes;
        }
    }

    void start_fade_in_animation() {
        // Set initial opacity to 0
        gtk_widget_set_opacity(window, 0.0);
        // Start fade-in timer with higher frequency for smoother animation
        fade_timeout_id = g_timeout_add(8, fade_in_timeout_static, this); // ~120fps for ultra smooth
    }

    gboolean fade_in_timeout() {
        static double opacity = 0.0;
        opacity += 0.12; // Even faster fade for instant responsiveness
        if (opacity >= 1.0) {
            opacity = 1.0;
            fade_in_complete = true;
            fade_timeout_id = 0;
            // Force redraw after fade-in completes
            gtk_widget_queue_draw(window);
            return FALSE; // Stop the timer
        }
        gtk_widget_set_opacity(window, opacity);
        return TRUE; // Continue animation
    }

    static std::string get_screenshot_path(int workspace_id) {
        return std::string("/tmp/workspace_previews/workspace_") + std::to_string(workspace_id) + ".png";
    }

    GdkPixbuf* create_workspace_thumbnail_from_path(const std::string& screenshot_path) {
        if (screenshot_path.empty() || !std::filesystem::exists(screenshot_path)) {
            return nullptr;
        }
        GError* error = nullptr;
        // Scale thumbnail size based on screen resolution
        int thumb_width = std::max(200, screen_width / 6);
        int thumb_height = std::max(112, static_cast<int>(thumb_width * 9.0 / 16.0)); // 16:9 aspect ratio
        GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_size(
            screenshot_path.c_str(), thumb_width, thumb_height, &error);
        if (error) {
            std::cerr << "Error creating thumbnail: " << error->message << std::endl;
            g_error_free(error);
            return nullptr;
        }
        return pixbuf;
    }

    std::vector<std::string> get_workspace_app_classes(int workspace_id) {
        std::vector<std::string> app_classes;
        std::string command;
        if (workspace_id == 13) {
            // Special workspace query - get apps from the Light special workspace
            command = "hyprctl clients -j 2>/dev/null | jq -r '.[] | select(.workspace.name == \"special:light\") | .class' 2>/dev/null";
        } else {
            // Regular workspace query
            command = "hyprctl clients -j 2>/dev/null | jq -r '.[] | select(.workspace.id == " +
                     std::to_string(workspace_id) + ") | .class' 2>/dev/null";
        }
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return app_classes;
        }
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            std::string app_class = buffer;
            if (!app_class.empty() && app_class.back() == '\n') {
                app_class.pop_back();
            }
            if (!app_class.empty()) {
                app_classes.push_back(app_class);
            }
        }
        pclose(pipe);
        return app_classes;
    }

    GdkPixbuf* get_app_icon(const std::string& app_class) {
        if (app_class.empty()) {
            return nullptr;
        }
        // Check cache first
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto it = theme_icon_cache.find(app_class);
            if (it != theme_icon_cache.end()) {
                return it->second ? g_object_ref(it->second) : nullptr;
            }
        }
        GtkIconTheme* theme = gtk_icon_theme_get_default();
        std::string icon_name = app_class;
        if (!gtk_icon_theme_has_icon(theme, icon_name.c_str())) {
            std::transform(icon_name.begin(), icon_name.end(), icon_name.begin(), ::tolower);
        }
        GError* error = nullptr;
        GdkPixbuf* pixbuf = gtk_icon_theme_load_icon(theme, icon_name.c_str(), app_icon_size,
                                                   GTK_ICON_LOOKUP_FORCE_SIZE, &error);
        if (error) {
            g_error_free(error);
            // Try fallbacks efficiently
            static const std::vector<std::string> fallbacks = {
                "application-x-executable", "application-default-icon", "application", "window", "folder"
            };
            for (const auto& fallback : fallbacks) {
                error = nullptr;
                pixbuf = gtk_icon_theme_load_icon(theme, fallback.c_str(), app_icon_size,
                                                GTK_ICON_LOOKUP_FORCE_SIZE, &error);
                if (pixbuf && !error) break;
                if (error) g_error_free(error);
            }
        }
        // Cache the result (even if null)
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            theme_icon_cache[app_class] = pixbuf ? g_object_ref(pixbuf) : nullptr;
        }
        return pixbuf;
    }

    // Lazy loading for tooltip data - only fetch when needed
    std::vector<std::string> get_workspace_apps(int workspace_id) {
        std::vector<std::string> apps;
        std::string command;
        if (workspace_id == 13) {
            // Special workspace query - get apps from the Light special workspace
            command = "hyprctl clients -j 2>/dev/null | jq -r '.[] | select(.workspace.name == \"special:light\") | .title' 2>/dev/null";
        } else {
            // Regular workspace query
            command = "hyprctl clients -j 2>/dev/null | jq -r '.[] | select(.workspace.id == " +
                     std::to_string(workspace_id) + ") | .title' 2>/dev/null";
        }
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) return apps;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            std::string title = buffer;
            if (!title.empty() && title.back() == '\n') {
                title.pop_back();
            }
            if (!title.empty()) {
                apps.push_back(title);
            }
        }
        pclose(pipe);
        return apps;
    }

    void create_tooltip() {
        tooltip_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_decorated(GTK_WINDOW(tooltip_window), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(tooltip_window), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(tooltip_window), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(tooltip_window), TRUE);
        gtk_window_set_type_hint(GTK_WINDOW(tooltip_window), GDK_WINDOW_TYPE_HINT_TOOLTIP);
        gtk_layer_init_for_window(GTK_WINDOW(tooltip_window));
        // --- CHANGE: Ensure tooltip is on the OVERLAY layer ---
        gtk_layer_set_layer(GTK_WINDOW(tooltip_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
        // --- END CHANGE ---
        gtk_layer_set_keyboard_mode(GTK_WINDOW(tooltip_window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 265);
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
        GtkStyleContext* context = gtk_widget_get_style_context(tooltip_window);
        gtk_style_context_add_class(context, "tooltip-window");
        gtk_widget_add_events(tooltip_window, GDK_KEY_PRESS_MASK);
        g_signal_connect(tooltip_window, "key-press-event", G_CALLBACK(WorkspaceSwitcher::on_key_press_static), this);
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
        // Enable compositing for smooth animations
        gtk_widget_set_app_paintable(window, TRUE);
        fixed = gtk_fixed_new();
        gtk_container_add(GTK_CONTAINER(window), fixed);
    }

    void setup_layer_shell() {
        gtk_layer_init_for_window(GTK_WINDOW(window));
        gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    }

    // Create buttons immediately without icons for fastest startup
    void create_workspace_buttons_minimal() {
        // Create regular workspaces 1-12 in circle
        for (int i = 1; i <= 12; i++) {
            double angle = (i - 1) * (2 * M_PI / 12) - (M_PI / 2);
            int x = center_x + radius * cos(angle);
            int y = center_y + radius * sin(angle);
            GtkWidget* button = gtk_button_new_with_label(std::to_string(i).c_str());
            gtk_widget_set_size_request(button, button_size, button_size);
            gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
            // Minimal styling - just add the basic classes
            GtkStyleContext* context = gtk_widget_get_style_context(button);
            gtk_style_context_add_class(context, "workspace-button");
            gtk_style_context_add_class(context, ("workspace-" + std::to_string(i)).c_str());
            // Essential event handling only
            g_signal_connect(button, "enter-notify-event", G_CALLBACK(WorkspaceSwitcher::on_button_enter_static), this);
            g_signal_connect(button, "leave-notify-event", G_CALLBACK(WorkspaceSwitcher::on_button_leave_static), this);
            gtk_widget_set_events(button, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
            g_object_set_data(G_OBJECT(button), "workspace", GINT_TO_POINTER(i));
            g_signal_connect(button, "clicked", G_CALLBACK(WorkspaceSwitcher::on_workspace_click_static), this);
            gtk_fixed_put(GTK_FIXED(fixed), button, x - button_size/2, y - button_size/2);
            // Store button reference for later icon updates
            workspace_buttons[i] = button;
        }
        // Create workspace 13 in the center - bigger than others
        GtkWidget* special_button = gtk_button_new_with_label("13");
        gtk_widget_set_size_request(special_button, special_button_size, special_button_size);
        gtk_button_set_relief(GTK_BUTTON(special_button), GTK_RELIEF_NONE);
        // Styling for special workspace
        GtkStyleContext* special_context = gtk_widget_get_style_context(special_button);
        gtk_style_context_add_class(special_context, "workspace-button");
        gtk_style_context_add_class(special_context, "workspace-13");
        // Event handling
        g_signal_connect(special_button, "enter-notify-event", G_CALLBACK(WorkspaceSwitcher::on_button_enter_static), this);
        g_signal_connect(special_button, "leave-notify-event", G_CALLBACK(WorkspaceSwitcher::on_button_leave_static), this);
        gtk_widget_set_events(special_button, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
        g_object_set_data(G_OBJECT(special_button), "workspace", GINT_TO_POINTER(13));
        g_signal_connect(special_button, "clicked", G_CALLBACK(WorkspaceSwitcher::on_workspace_click_static), this);
        // Position in center
        // The calculation `center_x - special_button_size/2` correctly centers the larger button
        gtk_fixed_put(GTK_FIXED(fixed), special_button, center_x - special_button_size/2, center_y - special_button_size/2);
        // Store button reference
        workspace_buttons[13] = special_button;
    }

    void show_tooltip(int workspace_id, gint x, gint y) {
        // Only show tooltip if it's been created (deferred creation)
        if (!tooltip_window) return;
        
        // Load tooltip data on-demand for better performance
        std::vector<std::string> apps = get_workspace_apps(workspace_id);
        
        // Only show thumbnail image if workspace has apps
        if (!apps.empty()) {
            std::string screenshot_path = get_screenshot_path(workspace_id);
            GdkPixbuf* thumbnail = create_workspace_thumbnail_from_path(screenshot_path);
            if (thumbnail) {
                gtk_image_set_from_pixbuf(GTK_IMAGE(tooltip_image), thumbnail);
                gtk_widget_show(tooltip_image);
                g_object_unref(thumbnail);
            } else {
                // Clear and hide image if screenshot doesn't exist
                gtk_image_clear(GTK_IMAGE(tooltip_image));
                gtk_widget_hide(tooltip_image);
            }
        } else {
            // Clear and hide image if workspace is empty (prevents showing previous workspace's image)
            gtk_image_clear(GTK_IMAGE(tooltip_image));
            gtk_widget_hide(tooltip_image);
        }
        
        std::string tooltip_text = "Workspace " + std::to_string(workspace_id);
        if (workspace_id == 13) {
            tooltip_text = "Special Workspace (Light)";
        }
        if (apps.empty()) {
            tooltip_text += "\nNothing";
        } else {
            tooltip_text += " (" + std::to_string(apps.size()) + " apps):";
            for (const auto& app : apps) {
                tooltip_text += "\n• " + app;
            }
        }
        gtk_label_set_text(GTK_LABEL(tooltip_label), tooltip_text.c_str());
        gtk_widget_show_all(tooltip_window);
        
        GtkRequisition tooltip_size;
        gtk_widget_get_preferred_size(tooltip_window, &tooltip_size, nullptr);
        
        // Custom positioning for workspace 13
        if (workspace_id == 13) {
            // Position to the right of the central button with padding
            x = center_x;  // 20px padding from button edge
            // Vertically centered: y passed in is the bottom edge of the button
            y = center_y + special_button_size / 2 + 20;
            
            // Ensure tooltip stays within screen bounds
            if (x + tooltip_size.width > screen_width) {
                x = screen_width - tooltip_size.width - 10;
            }
            if (y < 10) {
                y = 10;
            } else if (y + tooltip_size.height > screen_height - 10) {
                y = screen_height - tooltip_size.height - 10;
            }
        } else {
            // Original positioning logic for other workspaces
            x = x - tooltip_size.width / 2;
            y = y - tooltip_size.height - 40; // Above the button
            
            if (x + tooltip_size.width > screen_width) {
                x = screen_width - tooltip_size.width - 10;
            }
            if (x < 10) {
                x = 10;
            }
            if (y < 10) {
                y = y + tooltip_size.height + 80; // Position below if no room above
            }
            if (y + tooltip_size.height > screen_height) {
                y = screen_height - tooltip_size.height - 10;
            }
        }
        
        gtk_layer_set_margin(GTK_WINDOW(tooltip_window), GTK_LAYER_SHELL_EDGE_LEFT, x);
        gtk_layer_set_margin(GTK_WINDOW(tooltip_window), GTK_LAYER_SHELL_EDGE_TOP, y);
    }

    void hide_tooltip() {
        if (tooltip_window) {
            gtk_widget_hide(tooltip_window);
        }
    }

    bool is_currently_on_special_workspace() {
        // Check active window's workspace - this is more reliable than activeworkspace
        // because activeworkspace can return the workspace switcher window's workspace
        std::string command = "hyprctl -j activewindow 2>/dev/null | jq -r '.workspace.id' 2>/dev/null";
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return false;
        }
        char buffer[128];
        bool result = false;
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            std::string workspace_id_str = buffer;
            // Remove trailing newline and whitespace
            while (!workspace_id_str.empty() && (workspace_id_str.back() == '\n' || workspace_id_str.back() == '\r' || workspace_id_str.back() == ' ')) {
                workspace_id_str.pop_back();
            }
            // Special workspaces have negative IDs
            try {
                int workspace_id = std::stoi(workspace_id_str);
                result = (workspace_id < 0);
            } catch (...) {
                // If parsing fails, try checking workspace name as fallback
                pclose(pipe);
                std::string name_command = "hyprctl -j activewindow 2>/dev/null | jq -r '.workspace.name' 2>/dev/null";
                FILE* name_pipe = popen(name_command.c_str(), "r");
                if (name_pipe) {
                    char name_buffer[128];
                    if (fgets(name_buffer, sizeof(name_buffer), name_pipe) != nullptr) {
                        std::string workspace_name = name_buffer;
                        while (!workspace_name.empty() && (workspace_name.back() == '\n' || workspace_name.back() == '\r' || workspace_name.back() == ' ')) {
                            workspace_name.pop_back();
                        }
                        result = (workspace_name == "special:light" || workspace_name.find("special:") == 0);
                    }
                    pclose(name_pipe);
                }
                return result;
            }
        }
        pclose(pipe);
        return result;
    }

    void switch_workspace(int workspace_num) {
        bool is_on_special = is_currently_on_special_workspace();
        // Debug: Get active window workspace info (more accurate than activeworkspace)
        std::string debug_cmd = "hyprctl -j activewindow 2>/dev/null | jq -r '\"Window WS ID: \" + (.workspace.id | tostring) + \", Name: \" + .workspace.name' 2>/dev/null";
        FILE* debug_pipe = popen(debug_cmd.c_str(), "r");
        if (debug_pipe) {
            char debug_buffer[256];
            if (fgets(debug_buffer, sizeof(debug_buffer), debug_pipe) != nullptr) {
                std::string debug_info = debug_buffer;
                while (!debug_info.empty() && (debug_info.back() == '\n' || debug_info.back() == '\r')) {
                    debug_info.pop_back();
                }
                std::cerr << "DEBUG: Switching to workspace " << workspace_num << ", is_on_special=" << (is_on_special ? "true" : "false") << ", " << debug_info << std::endl;
            }
            pclose(debug_pipe);
        }
        
        if (workspace_num == 13) {
            // Special workspace toggle
            // Hyprland 0.55+ Lua backend rejects the legacy "dispatch <dispatcher> <args>"
            // CLI form outright (hyprctl dispatch workspace 5 -> Lua parse error). Use the
            // proper Lua dispatcher function instead of the exec_raw bridge (exec_raw
            // returns "ok" but silently no-ops for this dispatcher on this Hyprland build).
            std::string command = "hyprctl dispatch \"hl.dsp.workspace.toggle_special('light')\"";
            int result = system(command.c_str());
            if (result == 0) {
                std::cout << "Toggled special workspace light" << std::endl;
            } else {
                std::cerr << "Error toggling special workspace" << std::endl;
            }
        } else {
            // Regular workspace switch
            if (is_on_special) {
                // If currently on special workspace, toggle it off and switch to new workspace.
                // Run sequentially (not backgrounded) so the toggle completes before the switch,
                // using the proper Lua dispatcher function (exec_raw silently no-ops here).
                std::string toggle_cmd = "hyprctl dispatch \"hl.dsp.workspace.toggle_special('light')\"";
                system(toggle_cmd.c_str());
                std::string switch_cmd = "hyprctl dispatch \"hl.dsp.focus({workspace = '" + std::to_string(workspace_num) + "'})\"";
                int result = system(switch_cmd.c_str());
                if (result == 0) {
                    std::cout << "Toggled special workspace off and switched to workspace " << workspace_num << std::endl;
                } else {
                    std::cerr << "Error switching from special workspace to workspace " << workspace_num << std::endl;
                }
            } else {
                // Normal workspace switch
                std::string command = "hyprctl dispatch \"hl.dsp.focus({workspace = '" + std::to_string(workspace_num) + "'})\"";
                int result = system(command.c_str());
                if (result == 0) {
                    std::cout << "Switched to workspace " << workspace_num << std::endl;
                } else {
                    std::cerr << "Error switching to workspace " << workspace_num << std::endl;
                }
            }
        }
    }

    gboolean on_key_press(GdkEventKey* event) {
        // Fast key handling
        switch (event->keyval) {
            case GDK_KEY_Escape: gtk_main_quit(); return TRUE;
            case GDK_KEY_1: switch_workspace( 1); gtk_main_quit(); return TRUE;
            case GDK_KEY_2: switch_workspace( 2); gtk_main_quit(); return TRUE;
            case GDK_KEY_3: switch_workspace( 3); gtk_main_quit(); return TRUE;
            case GDK_KEY_4: switch_workspace( 4); gtk_main_quit(); return TRUE;
            case GDK_KEY_5: switch_workspace( 5); gtk_main_quit(); return TRUE;
            case GDK_KEY_6: switch_workspace( 6); gtk_main_quit(); return TRUE;
            case GDK_KEY_7: switch_workspace( 7); gtk_main_quit(); return TRUE;
            case GDK_KEY_8: switch_workspace( 8); gtk_main_quit(); return TRUE;
            case GDK_KEY_9: switch_workspace( 9); gtk_main_quit(); return TRUE;
            case GDK_KEY_0: switch_workspace(10); gtk_main_quit(); return TRUE;
            case GDK_KEY_minus:  switch_workspace(11); gtk_main_quit(); return TRUE;
            case GDK_KEY_equal:  switch_workspace(12); gtk_main_quit(); return TRUE;
            case GDK_KEY_BackSpace: switch_workspace(13); gtk_main_quit(); return TRUE; // Backspace for workspace 13
            default: return FALSE;
        }
    }

    void connect_signals() {
        g_signal_connect(window, "destroy", G_CALLBACK(WorkspaceSwitcher::on_destroy_static), this);
        g_signal_connect(window, "key-press-event", G_CALLBACK(WorkspaceSwitcher::on_key_press_static), this);
        gtk_widget_set_can_focus(window, TRUE);
        gtk_widget_grab_focus(window);
    }

    // Minimal CSS for instant startup
    void apply_minimal_css() {
        const char* minimal_css = R"(
            .workspace-button {
                background: transparent;
                border: none;
                border-radius: 50%;
                color: white;
                font-weight: bold;
                transition: transform 0.1s ease;
            }
            .workspace-button:hover {
                transform: scale(1.1);
            }
            window {
                background: transparent;
            }
        )";
        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_data(provider, minimal_css, -1, nullptr);
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
        g_object_unref(provider);
    }

    // Full CSS loaded asynchronously after startup
    void apply_full_css() {
        const char* css_data = R"(
            @keyframes pulse-glow {
                0% {
                    box-shadow:
                        inset 0 0 10px currentColor,
                        inset 0 0 20px currentColor,
                        0 0 15px currentColor,
                        0 0 30px currentColor,
                        0 0 45px currentColor;
                    transform: scale(1.0);
                }
                50% {
                    box-shadow:
                        inset 0 0 20px currentColor,
                        inset 0 0 40px currentColor,
                        0 0 25px currentColor,
                        0 0 50px currentColor,
                        0 0 75px currentColor;
                    transform: scale(1.05);
                }
                100% {
                    box-shadow:
                        inset 0 0 10px currentColor,
                        inset 0 0 20px currentColor,
                        0 0 15px currentColor,
                        0 0 30px currentColor,
                        0 0 45px currentColor;
                    transform: scale(1.0);
                }
            }
            @keyframes fade-in {
                from {
                    opacity: 0;
                    transform: scale(0.95);
                }
                to {
                    opacity: 1;
                    transform: scale(1.0);
                }
            }
            .workspace-icon {
                animation: fade-in 0.3s ease-out;
            }
            .workspace-button {
                background: transparent;
                border: none;
                border-radius: 50%;
                color: white;
                font-weight: bold;
                transition: all 0.1s cubic-bezier(0.25, 0.46, 0.45, 0.94);
                box-shadow: 0 0 0 transparent;
            }
            .workspace-button:hover {
                background: transparent;
                border: 0px;
                transform: scale(1.1);
            }
            .workspace-button:active {
                background: rgba(255, 255, 255, 0.2);
                transform: scale(0.95);
                transition: all 0.05s ease;
            }
            /* Individual workspace glow effects */
            .workspace-1:hover {
                color: rgb(173, 216, 230);
                box-shadow:
                    inset 0 0 15px rgba(173, 216, 230, 0.6),
                    inset 0 0 30px rgba(173, 216, 230, 0.4),
                    0 0 20px rgb(173, 216, 230),
                    0 0 40px rgb(173, 216, 230),
                    0 0 60px rgb(173, 216, 230);
                animation: pulse-glow 1.5s infinite ease-in-out;
            }
            .workspace-2:hover {
                color: rgb(0, 100, 255);
                box-shadow:
                    inset 0 0 15px rgba(0, 100, 255, 0.6),
                    inset 0 0 30px rgba(0, 100, 255, 0.4),
                    0 0 20px rgb(0, 100, 255),
                    0 0 40px rgb(0, 100, 255),
                    0 0 60px rgb(0, 100, 255);
                animation: pulse-glow 1.5s infinite ease-in-out;
            }
            .workspace-3:hover {
                color: rgb(255, 215, 0);
                box-shadow:
                    inset 0 0 15px rgba(255, 215, 0, 0.6),
                    inset 0 0 30px rgba(255, 215, 0, 0.4),
                    0 0 20px rgb(255, 215, 0),
                    0 0 40px rgb(255, 215, 0),
                    0 0 60px rgb(255, 215, 0);
                animation: pulse-glow 1.5s infinite ease-in-out;
            }
            .workspace-4:hover {
                color: rgb(255, 235, 164);
                box-shadow:
                    inset 0 0 15px rgba(255, 255, 224, 0.6),
                    inset 0 0 30px rgba(255, 255, 224, 0.4),
                    0 0 20px rgb(255, 235, 164),
                    0 0 40px rgb(255, 235, 164),
                    0 0 60px rgb(255, 235, 164);
                animation: pulse-glow 1.5s infinite ease-in-out;
            }
            .workspace-5:hover {
                color: rgb(233, 28, 32);
                box-shadow:
                    inset 0 0 15px rgba(203, 28, 32, 0.6),
                    inset 0 0 30px rgba(203, 28, 32, 0.4),
                    0 0 20px rgb(233, 28, 32),
                    0 0 40px rgb(233, 28, 32),
                    0 0 60px rgb(233, 28, 32);
                animation: pulse-glow 1.5s infinite ease-in-out;
            }
            .workspace-6:hover {
                color: rgb(144, 238, 144);
                box-shadow:
                    inset 0 0 15px rgba(144, 238, 144, 0.6),
                    inset 0 0 30px rgba(144, 238, 144, 0.4),
                    0 0 20px rgb(144, 238, 144),
                    0 0 40px rgb(144, 238, 144),
                    0 0 60px rgb(144, 238, 144);
                animation: pulse-glow 1.5s infinite ease-in-out;
            }
            .workspace-7:hover {
                color: rgb(255, 182, 193);
                box-shadow:
                    inset 0 0 15px rgba(255, 182, 193, 0.6),
                    inset 0 0 30px rgba(255, 182, 193, 0.4),
                    0 0 20px rgb(255, 182, 193),
                    0 0 40px rgb(255, 182, 193),
                    0 0 60px rgb(255, 182, 193);
                animation: pulse-glow 1.5s infinite ease-in-out;
            }
            .workspace-8:hover {
                color: rgb(255, 255, 255);
                box-shadow:
                    inset 0 0 15px rgba(255, 255, 255, 0.6),
                    inset 0 0 30px rgba(255, 255, 255, 0.4),
                    0 0 20px rgb(255, 255, 255),
                    0 0 40px rgb(255, 255, 255),
                    0 0 60px rgb(255, 255, 255);
                animation: pulse-glow 1.5s infinite ease-in-out;
            }
            .workspace-9:hover {
                color: rgb(0, 255, 0);
                box-shadow:
                    inset 0 0 15px rgba(0, 255, 0, 0.6),
                    inset 0 0 30px rgba(0, 255, 0, 0.4),
                    0 0 20px rgb(0, 255, 0),
                    0 0 40px rgb(0, 255, 0),
                    0 0 60px rgb(0, 255, 0);
                animation: pulse-glow 1.5s infinite ease-in-out;
            }
            .workspace-10:hover {
                color: rgb(135, 206, 235);
                box-shadow:
                    inset 0 0 15px rgba(135, 206, 235, 0.6),
                    inset 0 0 30px rgba(135, 206, 235, 0.4),
                    0 0 20px rgb(135, 206, 235),
                    0 0 40px rgb(135, 206, 235),
                    0 0 60px rgb(135, 206, 235);
                animation: pulse-glow 1.5s infinite ease-in-out;
            }
            .workspace-11:hover {
                color: rgb(248, 248, 255);
                box-shadow:
                    inset 0 0 15px rgba(248, 248, 255, 0.6),
                    inset 0 0 30px rgba(248, 248, 255, 0.4),
                    0 0 20px rgb(248, 248, 255),
                    0 0 40px rgb(248, 248, 255),
                    0 0 60px rgb(248, 248, 255);
                animation: pulse-glow 1.5s infinite ease-in-out;
            }
            .workspace-12:hover {
                color: rgb(255, 192, 203);
                box-shadow:
                    inset 0 0 15px rgba(255, 192, 203, 0.6),
                    inset 0 0 30px rgba(255, 192, 203, 0.4),
                    0 0 20px rgb(255, 192, 203),
                    0 0 40px rgb(255, 192, 203),
                    0 0 60px rgb(255, 192, 203);
                animation: pulse-glow 1.5s infinite ease-in-out;
            }
            .workspace-13:hover {
                color: rgb(255, 20, 147);
                box-shadow:
                    inset 0 0 20px rgba(255, 20, 147, 0.7),
                    inset 0 0 40px rgba(255, 20, 147, 0.5),
                    0 0 30px rgb(255, 20, 147),
                    0 0 60px rgb(255, 20, 147),
                    0 0 90px rgb(255, 20, 147);
                animation: pulse-glow 1.2s infinite ease-in-out;
            }
            .app-icon {
                background: transparent;
                border-radius: 10px;
                opacity: 0.8;
                transition: all 0.1s cubic-bezier(0.25, 0.46, 0.45, 0.94);
            }
            .app-icon:hover {
                opacity: 1.0;
                transform: scale(1.1);
                box-shadow: 0 0 10px rgba(255, 255, 255, 0.5);
            }
            .tooltip-window {
                background: rgba(0, 0, 0, 0);
                border: 1px solid rgba(255, 255, 255, 0);
                border-radius: 16px;
                color: white;
                font-family: LightOSNew12;
                font-size: 14px;
                text-shadow: 1px 1px 3px rgba(0, 0, 0, 0.8);
            }
            window {
                background: transparent;
            }
        )";
        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_data(provider, css_data, -1, nullptr);
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1 // Higher priority to override minimal CSS
        );
        g_object_unref(provider);
    }

    void run() {
        gtk_main();
    }
};

// Static callbacks
gboolean WorkspaceSwitcher::load_workspace_icons_async_static(gpointer user_data) {
    WorkspaceSwitcher* self = static_cast<WorkspaceSwitcher*>(user_data);
    return self->load_workspace_icons_async();
}

gboolean WorkspaceSwitcher::load_app_icons_async_static(gpointer user_data) {
    WorkspaceSwitcher* self = static_cast<WorkspaceSwitcher*>(user_data);
    return self->load_app_icons_async();
}

gboolean WorkspaceSwitcher::fade_in_timeout_static(gpointer user_data) {
    WorkspaceSwitcher* self = static_cast<WorkspaceSwitcher*>(user_data);
    return self->fade_in_timeout();
}

// --- MODIFIED on_button_enter_static ---
gboolean WorkspaceSwitcher::on_button_enter_static(GtkWidget* button, GdkEventCrossing* event, gpointer user_data) {
    (void)event;
    WorkspaceSwitcher* self = static_cast<WorkspaceSwitcher*>(user_data);
    int workspace = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "workspace"));
    // Only show tooltip if fade-in is complete for better performance
    if (!self->fade_in_complete) return FALSE;
    gint tooltip_x, tooltip_y;
    if (workspace == 13) {
        // For workspace 13 (center), position tooltip anchor point BELOW the center button
        tooltip_x = self->center_x;
        // Anchor point is at the bottom edge of the special button
        tooltip_y = self->center_y + self->special_button_size/2;
    } else {
        // For regular workspaces, use button allocation and position tooltip anchor above
        GtkAllocation allocation;
        gtk_widget_get_allocation(button, &allocation);
        gint win_x, win_y;
        gtk_window_get_position(GTK_WINDOW(gtk_widget_get_toplevel(button)), &win_x, &win_y);
        tooltip_x = win_x + allocation.x + allocation.width / 2;
        // Anchor point is slightly above the button
        tooltip_y = win_y + allocation.y - 10;
    }
    self->show_tooltip(workspace, tooltip_x, tooltip_y);
    return FALSE;
}
// --- END MODIFICATION ---

gboolean WorkspaceSwitcher::on_button_leave_static(GtkWidget* button, GdkEventCrossing* event, gpointer user_data) {
    (void)button;
    (void)event;
    WorkspaceSwitcher* self = static_cast<WorkspaceSwitcher*>(user_data);
    self->hide_tooltip();
    return FALSE;
}

void WorkspaceSwitcher::on_workspace_click_static(GtkWidget* button, gpointer user_data) {
    WorkspaceSwitcher* self = static_cast<WorkspaceSwitcher*>(user_data);
    int workspace = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "workspace"));
    self->switch_workspace(workspace);
    gtk_main_quit();
}

gboolean WorkspaceSwitcher::on_key_press_static(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
    (void)widget;
    WorkspaceSwitcher* self = static_cast<WorkspaceSwitcher*>(user_data);
    return self->on_key_press(event);
}

void WorkspaceSwitcher::on_destroy_static(GtkWidget* widget, gpointer user_data) {
    (void)widget;
    WorkspaceSwitcher* self = static_cast<WorkspaceSwitcher*>(user_data);
    self->cleanup_caches();
    gtk_main_quit();
}

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);
    // Optimize GTK settings for maximum performance
    g_object_set(gtk_settings_get_default(),
                 "gtk-enable-animations", TRUE,
                 "gtk-animation-duration", 5, // Ultra-fast animations
                 "gtk-double-click-time", 200, // Faster double-clicks
                 nullptr);
    WorkspaceSwitcher app;
    app.run();
    return 0;
}
