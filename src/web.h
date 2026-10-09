// Web page and JSON API. Read-only toward the furnace: settings change how the
// board reports, never what the furnace does.
#pragma once

namespace web {

void begin();
void loop();  // pushes live state to WebSocket clients

}  // namespace web
