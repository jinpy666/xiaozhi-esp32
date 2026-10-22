# Local Components

## esp-wifi-connect

Vendored OEM fork of [78/esp-wifi-connect](https://github.com/78/esp-wifi-connect)
(upstream base: `cc10389`, v3.3.1). Source of truth for development:

- Fork: https://github.com/jinpy666/esp-wifi-connect
- Local working copy: `~/btroot/esp-wifi-connect`

A project component takes precedence over the registry-managed component with
the same name, so `main/idf_component.yml` keeps the upstream coordinate
(`78/esp-wifi-connect: ~3.3.1`) as the version record while the build uses this
copy. Do **not** edit `managed_components/78__esp-wifi-connect/` — it is
generated and will be replaced.

### OEM additions (on top of upstream)

| File | Change |
| --- | --- |
| `include/wifi_portal_brand.h` | `WifiPortalBrand` payload struct (`config_json`, logo bytes/MIME) |
| `include/wifi_manager.h` | `WifiManagerConfig::portal_brand` |
| `wifi_manager.cc` | Forwards `portal_brand` to the config AP |
| `include/wifi_configuration_ap.h`, `wifi_configuration_ap.cc` | `SetPortalBrand()`; `GET /brand.js`, `GET /brand-logo`; gzip-aware page handlers |
| `assets/wifi_configuration*.html` | Brand hook (`/brand.js` + apply script), forced `data-theme`, stable element ids |
| `CMakeLists.txt` | Portal page slot (`product/brand_assets/portal/`), pre-gzip'd embedding |

Default behavior is unchanged when no brand pack is staged: the stock pages are
embedded and no `/brand.js` or `/brand-logo` endpoints are registered.

### Syncing upstream

```sh
cd ~/btroot/esp-wifi-connect
git fetch upstream            # or the 78 remote
git merge upstream/main       # keep the OEM diff minimal
rsync -a --delete --exclude .git --exclude .github --exclude .gitignore \
      ~/btroot/esp-wifi-connect/ \
      ~/btroot/xiaozhi-esp32/components/esp-wifi-connect/
rm -rf ~/btroot/xiaozhi-esp32/components/esp-wifi-connect/{.github,.gitignore}
diff -r --exclude .git --exclude .github --exclude .gitignore \
      ~/btroot/esp-wifi-connect \
      ~/btroot/xiaozhi-esp32/components/esp-wifi-connect   # must be identical
```

The OEM additions above are deliberately generic and are good candidates for an
upstream pull request.
