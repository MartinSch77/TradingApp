# The eToro API as used here

@page etoroapi Endpoints, payload quirks and troubleshooting
@tableofcontents

The endpoints this application actually calls, and the behaviours that cost time to
discover. Moved out of the README, which does not need an endpoint list on its front
page.

All requests send the documented `x-api-key`, `x-user-key`, and per-request
`x-request-id` (UUID) headers. See [`src/services/EtoroClient.cpp`](../src/services/EtoroClient.cpp).

| Purpose            | Method & path | Verified live |
|--------------------|---------------|:---:|
| Resolve instrument | `GET /v1/market-data/search?internalSymbolFull=SPX500&fields=…` | ✅ (SPX500 = id `27`) |
| Chart history      | `GET /v1/market-data/instruments/{id}/history/candles/{dir}/{interval}/{count}` | ✅ |
| Live price         | `GET /v1/market-data/instruments/rates?instrumentIds={id}` | ✅ |
| Open position      | `POST /v2/trading/execution/{demo\|}/orders` (`orderType: mkt`) | ⚠️ needs trading token |
| Limit order        | `POST /v2/trading/execution/{demo\|}/orders` (`orderType: mit` + `triggerRate`) | ⚠️ needs trading token |
| Order status       | `GET /v2/trading/info/{demo\|}/orders:lookup?orderId={id}` | ⚠️ needs trading token |
| Cancel limit order | `DELETE /v2/trading/execution/{demo\|}/orders/{orderId}` | ⚠️ needs trading token |
| Close position     | `POST /v1/trading/execution/{demo\|}/market-close-orders/positions/{positionId}` | ⚠️ needs trading token |
| Portfolio          | `GET /v1/trading/info/{demo\|}/portfolio` | ⚠️ needs trading token |

Confirmed real quirks (already handled in code):
- Search **ignores** a free-text `query=`; filter with **`internalSymbolFull`**. The
  first result row `{"instrumentId":-100000}` is a placeholder and is skipped.
- Candles are **nested**: `{ candles: [ { instrumentId, candles: [ {fromDate,open,high,low,close} ] } ] }`.
- Rates fields are `lastExecution` / `bid` / `ask` (no `currentRate`/`close`).

**Note on JSON schemas.** Order and portfolio response schemas are only visible in
the authenticated reference. The client parses responses **defensively** (tries
several field names, unwraps `data`) and logs anything it cannot parse to the
Activity panel. Adjust the `pick(...)` key lists in `src/services/EtoroClient.cpp`, and the
candle interval/direction/count near the top of `EtoroClient.h`
(`m_candleInterval`, `m_candleDirection`, `m_candleCount`) if needed.

## Troubleshooting: HTTP 403 `InsufficientPermissions`

Market-data calls succeed but trading/portfolio calls return
`403 {"errorCode":"InsufficientPermissions"}`. This means your API token is an
**`UnregisteredApplication`** token without trading scope. Register/approve the
application in the API portal and regenerate the keys to get trading + portfolio
access; no code change is needed afterwards.
