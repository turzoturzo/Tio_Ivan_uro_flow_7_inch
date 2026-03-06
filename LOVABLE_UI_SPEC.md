# Lovable-to-LVGL UI Spec (March 6, 2026)

This project now uses the Lovable `milli-flow-tale` output as the source of
truth for visual direction, with firmware-appropriate adaptations.

## 1) Design Tokens

- Background: `#0A0A0A`
- Surface/card: `#121212`
- Border: `#222222`
- Primary accent: `#00E660`
- Foreground text: `#F2F2F2`
- Muted text: `#808080`
- Error/destructive: `#E5484D`
- Shape language: low radius (`4px` equivalent)
- Contrast: high, neon-on-black

Typography mapping (LVGL built-ins):

- Display headline: `lv_font_montserrat_48`
- Section/value emphasis: `lv_font_montserrat_32` / `24`
- Utility/meta text: `lv_font_montserrat_14`

## 2) Screen Mapping

Lovable screen -> Firmware state:

- Splash -> `UIState::BOOT`
- ScaleConnected -> `UIState::READY`
- LiveMeasurement -> `UIState::ACTIVE`
- Success -> `UIState::SUCCESS`
- Failure -> `UIState::ERROR`
- Upload in progress -> `UIState::SYNCING`

## 3) Implemented Parity

- Branded header treatment (`Mongo` + green `Flo`) across dark screens.
- Boot screen two-column layout:
  - Left cards: WiFi network + export entry.
  - Right card: connection countdown + progress bar.
- Connected screen:
  - Full green field, centered confirmation, tap-to-start affordance.
- Live measurement screen:
  - Header with recording indicator and telemetry.
  - Full-width chart area with dark grid + neon line.
  - Footer data point count.
  - Overlay countdown when weight change stalls.
- Success screen:
  - Green confirmation layout with sync message and countdown rail.
- Syncing screen:
  - Dedicated sync state with clear status text.
- Error screen:
  - Destructive red fallback styling.

## 4) Data Plumbing Added

- Boot WiFi card now receives real SSID from NVS/config when available.
- Live screen now receives:
  - cumulative grams
  - elapsed seconds
  - weight-removal countdown
  - data point count
- Ended/waiting session states now map to the success screen.

## 5) Still Not Final Branding

- Brand naming/copy (`MongoFlo`, legal footer text, version strings) remains a
  draft and should be replaced with finalized product copy.
- Font fidelity is approximate due to LVGL firmware font constraints
  (Space Grotesk / JetBrains Mono not embedded yet).
