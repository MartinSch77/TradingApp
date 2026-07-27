#ifndef TRADINGAPP_UI_PALETTE_H
#define TRADINGAPP_UI_PALETTE_H

#include <QColor>

// The app-wide signal colors, shared by every ui widget so BUY/SELL/neutral
// always render identically (the same values the HTML labels hardcode as
// #25b563 / #e35555 / #9a9a9a). Inline variables: one entity across all TUs.
namespace trading::ui {

inline const QColor kGreen(0x25, 0xb5, 0x63);  // BUY / long / profit
inline const QColor kRed(0xe3, 0x55, 0x55);    // SELL / short / loss
inline const QColor kGrey(0x9a, 0x9a, 0x9a);   // neutral / no signal
inline const QColor kAmber(0xe0, 0xb0, 0x00);  // warning / stale / caution (#e0b000)

}  // namespace trading::ui

#endif  // TRADINGAPP_UI_PALETTE_H
