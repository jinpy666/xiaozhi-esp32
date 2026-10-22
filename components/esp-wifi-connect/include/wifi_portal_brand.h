#ifndef _WIFI_PORTAL_BRAND_H_
#define _WIFI_PORTAL_BRAND_H_

#include <cstddef>

/**
 * WifiPortalBrand - Branding payload for the configuration portal web UI.
 *
 * The application fills this struct from its own product branding settings.
 * When `config_json` is empty the portal serves the stock page and registers
 * no brand endpoints, keeping the default behavior unchanged.
 *
 * The portal exposes:
 *   GET /brand.js    -> "window.BRAND_CONFIG = <config_json>"
 *   GET /brand-logo  -> raw logo_data with logo_mime (only when logo is set)
 */
struct WifiPortalBrand {
    // JSON object body for window.BRAND_CONFIG (without the assignment).
    // Recognized fields (all optional): name, accent, theme ("dark"|"light"),
    // css_vars (object of CSS custom properties), i18n (per-language overrides
    // of the page translation table), sections (set false to hide UI blocks:
    // advanced_tab, tx_power, sleep_toggle, saved_list, language_switch),
    // notice, footer, logo (URL, e.g. "/brand-logo").
    std::string config_json;

    // Raw bytes of the logo asset (e.g. an EMBED_FILES symbol in flash).
    const char* logo_data = nullptr;
    size_t logo_size = 0;
    const char* logo_mime = "image/svg+xml";
};

#endif // _WIFI_PORTAL_BRAND_H_
