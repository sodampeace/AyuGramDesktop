// Per-chat opt-outs: chats whose deleted messages are not kept, and chats
// left out of the local Korean search index. Both are toggled from the chat
// menu and edited as a list in AyuGram settings.
#pragma once

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

namespace AyuExclusions {

enum class Kind {
	DeletedMessages,
	SearchIndex,
};

[[nodiscard]] bool IsExcluded(Kind kind, not_null<PeerData*> peer);
void SetExcluded(Kind kind, not_null<PeerData*> peer, bool excluded);

// Multi-select chat picker that saves straight into the settings.
void ShowBox(not_null<Window::SessionController*> controller, Kind kind);

} // namespace AyuExclusions
