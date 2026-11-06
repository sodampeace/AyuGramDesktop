// Local message index for Korean search.
//
// Telegram's server search splits text into words and matches word starts,
// so "한국" misses "한국에서봤어요". Every message the client sees is kept in
// a small SQLite table here and searched with substring matching.
#pragma once

class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace AyuLocalSearch {

struct Result {
	FullMsgId id;
	TimeId date = 0;
};

// True when the query contains Hangul, i.e. when local results are useful.
[[nodiscard]] bool Wanted(const QString &query);

// Chats picked in AyuGram settings are never written to the index.
[[nodiscard]] bool Excluded(PeerId peer);
// Removes everything stored for a chat (used when it gets excluded).
void Forget(PeerId peer);

void Index(not_null<HistoryItem*> item);
void IndexRaw(
	not_null<Main::Session*> session,
	PeerId peer,
	MsgId id,
	TimeId date,
	const QString &text);

// Newest first. `inPeer` empty means all chats of this account.
// Runs off the main thread; `done` is called on the main thread.
void Search(
	not_null<Main::Session*> session,
	const QString &query,
	PeerId inPeer,
	int limit,
	Fn<void(std::vector<Result>)> done);

// Downloads the whole history of a chat into the index, slowly, in the
// background. Calls `progress(indexed)` after every page, `done` at the end.
void Backfill(
	not_null<PeerData*> peer,
	Fn<void(int)> progress,
	Fn<void()> done);
[[nodiscard]] bool BackfillRunning(not_null<PeerData*> peer);
void CancelBackfill(not_null<PeerData*> peer);

// Backfills a list of chats (e.g. a whole folder) one after another, so the
// server sees a single slow download. `done(messages)` at the end.
void BackfillMany(
	not_null<Main::Session*> session,
	std::vector<not_null<PeerData*>> peers,
	Fn<void(int)> done);
[[nodiscard]] bool BackfillManyRunning();
void CancelBackfillMany();

} // namespace AyuLocalSearch
