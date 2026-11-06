// Local message index for Korean search. See local_search.h.
#include "ayu/features/local_search/local_search.h"

#include "ayu/libs/sqlite/sqlite3.h"
#include "apiwrap.h"
#include "ayu/ayu_settings.h"
#include "base/timer.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/text/text_entity.h"

#include <QtCore/QCoreApplication>

namespace AyuLocalSearch {
namespace {

constexpr auto kFlushDelay = crl::time(1000);
constexpr auto kBackfillPage = 100;
constexpr auto kBackfillDelay = crl::time(1500);
constexpr auto kBackfillFloodPause = crl::time(30000);

struct Pending {
	int64 account = 0;
	int64 peer = 0;
	int64 id = 0;
	int64 date = 0;
	QByteArray norm;
};

[[nodiscard]] QString Normalize(const QString &text) {
	return TextUtilities::NameSortKey(text);
}

class Database final {
public:
	static Database &Instance() {
		static auto instance = Database();
		return instance;
	}

	void queue(Pending &&row) {
		_pending.push_back(std::move(row));
		if (!_flush.isActive()) {
			_flush.callOnce(kFlushDelay);
		}
	}

	void forget(int64 peer) {
		_pending.erase(ranges::remove(_pending, peer, &Pending::peer), end(_pending));
		if (!_db) {
			return;
		}
		sqlite3_stmt *statement = nullptr;
		for (const auto sql : {
				"DELETE FROM messages WHERE peer = ?",
				"DELETE FROM backfill WHERE peer = ?" }) {
			if (sqlite3_prepare_v2(_db, sql, -1, &statement, nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(statement, 1, peer);
				sqlite3_step(statement);
				sqlite3_finalize(statement);
			}
			statement = nullptr;
		}
	}

	struct Progress {
		int64 offsetId = 0;
		bool done = false;
	};

	// Saved progress, or a guess for chats downloaded before progress was
	// saved: the oldest indexed message, when there are many of them.
	[[nodiscard]] Progress progress(int64 account, int64 peer) {
		flush();
		auto result = Progress();
		if (!_db) {
			return result;
		}
		sqlite3_stmt *statement = nullptr;
		const auto sql = "SELECT offset_id, done FROM backfill"
			" WHERE account = ? AND peer = ?";
		if (sqlite3_prepare_v2(_db, sql, -1, &statement, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(statement, 1, account);
			sqlite3_bind_int64(statement, 2, peer);
			if (sqlite3_step(statement) == SQLITE_ROW) {
				result.offsetId = sqlite3_column_int64(statement, 0);
				result.done = sqlite3_column_int64(statement, 1) != 0;
				sqlite3_finalize(statement);
				return result;
			}
			sqlite3_finalize(statement);
		}
		constexpr auto kGuessMinimum = 500;
		const auto guess = "SELECT MIN(id), COUNT(*) FROM messages"
			" WHERE account = ? AND peer = ?";
		if (sqlite3_prepare_v2(_db, guess, -1, &statement, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(statement, 1, account);
			sqlite3_bind_int64(statement, 2, peer);
			if (sqlite3_step(statement) == SQLITE_ROW
				&& sqlite3_column_int64(statement, 1) >= kGuessMinimum) {
				result.offsetId = sqlite3_column_int64(statement, 0);
			}
			sqlite3_finalize(statement);
		}
		return result;
	}

	void saveProgress(int64 account, int64 peer, int64 offsetId, bool done) {
		flush(); // Messages first, so saved progress never runs ahead.
		if (!_db) {
			return;
		}
		sqlite3_stmt *statement = nullptr;
		const auto sql = "INSERT OR REPLACE INTO backfill"
			" (account, peer, offset_id, done) VALUES (?, ?, ?, ?)";
		if (sqlite3_prepare_v2(_db, sql, -1, &statement, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(statement, 1, account);
			sqlite3_bind_int64(statement, 2, peer);
			sqlite3_bind_int64(statement, 3, offsetId);
			sqlite3_bind_int(statement, 4, done ? 1 : 0);
			sqlite3_step(statement);
			sqlite3_finalize(statement);
		}
	}

	// Writes queued rows so a search right after sees them.
	void flushNow() {
		flush();
	}

	[[nodiscard]] bool opened() const {
		return _db != nullptr;
	}

	[[nodiscard]] static QByteArray Path() {
		return (cWorkingDir() + u"tdata/ayu_search.db"_q).toUtf8();
	}

private:
	Database() : _flush([=] { flush(); }) {
		// Write the last second of messages before the app goes away.
		QObject::connect(
			QCoreApplication::instance(),
			&QCoreApplication::aboutToQuit,
			[=] { flush(); });
		const auto path = Path();
		if (sqlite3_open(path.constData(), &_db) != SQLITE_OK) {
			LOG(("Local search: could not open %1").arg(QString::fromUtf8(path)));
			sqlite3_close(_db);
			_db = nullptr;
			return;
		}
		exec("PRAGMA journal_mode=WAL");
		exec("PRAGMA synchronous=NORMAL");
		exec("CREATE TABLE IF NOT EXISTS messages ("
			"account INTEGER NOT NULL, peer INTEGER NOT NULL, "
			"id INTEGER NOT NULL, date INTEGER NOT NULL, "
			"norm TEXT NOT NULL, "
			"PRIMARY KEY (account, peer, id)) WITHOUT ROWID");
		exec("CREATE INDEX IF NOT EXISTS messages_date "
			"ON messages (account, date)");
		// Where each chat's history download stopped, so it can resume.
		exec("CREATE TABLE IF NOT EXISTS backfill ("
			"account INTEGER NOT NULL, peer INTEGER NOT NULL, "
			"offset_id INTEGER NOT NULL, done INTEGER NOT NULL, "
			"PRIMARY KEY (account, peer)) WITHOUT ROWID");
	}

	void exec(const char *sql) {
		char *error = nullptr;
		if (sqlite3_exec(_db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
			LOG(("Local search SQL error: %1").arg(error ? error : "?"));
			sqlite3_free(error);
		}
	}

	void flush() {
		_flush.cancel();
		if (!_db || _pending.empty()) {
			_pending.clear();
			return;
		}
		sqlite3_stmt *statement = nullptr;
		const auto sql = "INSERT OR REPLACE INTO messages "
			"(account, peer, id, date, norm) VALUES (?, ?, ?, ?, ?)";
		if (sqlite3_prepare_v2(_db, sql, -1, &statement, nullptr) != SQLITE_OK) {
			_pending.clear();
			return;
		}
		exec("BEGIN");
		for (const auto &row : _pending) {
			sqlite3_bind_int64(statement, 1, row.account);
			sqlite3_bind_int64(statement, 2, row.peer);
			sqlite3_bind_int64(statement, 3, row.id);
			sqlite3_bind_int64(statement, 4, row.date);
			sqlite3_bind_text(
				statement,
				5,
				row.norm.constData(),
				row.norm.size(),
				SQLITE_STATIC);
			sqlite3_step(statement);
			sqlite3_reset(statement);
		}
		exec("COMMIT");
		sqlite3_finalize(statement);
		_pending.clear();
	}

	sqlite3 *_db = nullptr;
	std::vector<Pending> _pending;
	base::Timer _flush;
};

// Runs on a worker thread with its own read connection (WAL allows it).
std::vector<Result> RunSearch(
		sqlite3 *db,
		int64 account,
		const QStringList &words,
		int64 inPeer,
		int limit) {
	auto result = std::vector<Result>();
	if (!db || words.isEmpty()) {
		return result;
	}
	auto sql = QByteArray("SELECT peer, id, date FROM messages"
		" WHERE account = ?");
	if (inPeer) {
		sql += " AND peer = ?";
	}
	for (auto i = 0; i != words.size(); ++i) {
		sql += " AND instr(norm, ?) > 0";
	}
	sql += " ORDER BY date DESC LIMIT ?";
	sqlite3_stmt *statement = nullptr;
	if (sqlite3_prepare_v2(db, sql.constData(), -1, &statement, nullptr)
		!= SQLITE_OK) {
		return result;
	}
	auto index = 1;
	sqlite3_bind_int64(statement, index++, account);
	if (inPeer) {
		sqlite3_bind_int64(statement, index++, inPeer);
	}
	auto keep = std::vector<QByteArray>();
	keep.reserve(words.size());
	for (const auto &word : words) {
		keep.push_back(word.toUtf8());
		sqlite3_bind_text(
			statement,
			index++,
			keep.back().constData(),
			keep.back().size(),
			SQLITE_STATIC);
	}
	sqlite3_bind_int(statement, index++, limit);
	while (sqlite3_step(statement) == SQLITE_ROW) {
		result.push_back({
			.id = FullMsgId(
				PeerId(PeerIdHelper(sqlite3_column_int64(statement, 0))),
				MsgId(sqlite3_column_int64(statement, 1))),
			.date = TimeId(sqlite3_column_int64(statement, 2)),
		});
	}
	sqlite3_finalize(statement);
	return result;
}

// Same form as getDialogIdFromPeer: users positive, groups and channels negative.
[[nodiscard]] int64 DialogId(PeerId peer) {
	const auto bare = int64(peer.value & PeerId::kChatTypeMask);
	return peerIsUser(peer) ? bare : -bare;
}

struct BackfillState {
	explicit BackfillState(not_null<PeerData*> peer) : peer(peer) {
	}

	not_null<PeerData*> peer;
	MsgId offsetId = 0;
	int indexed = 0;
	mtpRequestId requestId = 0;
	base::Timer timer;
	Fn<void(int)> progress;
	Fn<void()> done;
};

base::flat_map<not_null<PeerData*>, std::unique_ptr<BackfillState>> Backfills;

void BackfillNext(not_null<PeerData*> peer);

void BackfillFinish(not_null<PeerData*> peer) {
	const auto i = Backfills.find(peer);
	if (i == Backfills.end()) {
		return;
	}
	const auto done = std::move(i->second->done);
	Backfills.erase(i);
	if (done) {
		done();
	}
}

void BackfillNext(not_null<PeerData*> peer) {
	const auto i = Backfills.find(peer);
	if (i == Backfills.end()) {
		return;
	}
	const auto state = i->second.get();
	const auto session = &peer->session();
	state->requestId = session->api().request(MTPmessages_GetHistory(
		peer->input(),
		MTP_int(state->offsetId.bare),
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(kBackfillPage),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		state->requestId = 0;
		auto count = 0;
		auto lowest = MsgId();
		result.match([&](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &data) {
			for (const auto &message : data.vmessages().v) {
				message.match([&](const MTPDmessage &data) {
					IndexRaw(
						session,
						peer->id,
						data.vid().v,
						data.vdate().v,
						qs(data.vmessage()));
				}, [](const auto &) {
				});
				const auto id = IdFromMessage(message);
				if (!lowest || id < lowest) {
					lowest = id;
				}
				++count;
			}
		});
		state->indexed += count;
		if (state->progress) {
			state->progress(state->indexed);
		}
		const auto account = int64(session->userId().bare);
		if (!count || !lowest || (state->offsetId && lowest >= state->offsetId)) {
			Database::Instance().saveProgress(
				account,
				int64(peer->id.value),
				int64(state->offsetId.bare),
				true);
			BackfillFinish(peer);
			return;
		}
		state->offsetId = lowest;
		Database::Instance().saveProgress(
			account,
			int64(peer->id.value),
			int64(lowest.bare),
			false);
		state->timer.callOnce(kBackfillDelay);
	}).fail([=](const MTP::Error &error) {
		state->requestId = 0;
		// FLOOD_WAIT and friends: wait and try the same page again.
		const auto type = error.type();
		if (type.startsWith(u"FLOOD_WAIT_"_q)
			|| type.startsWith(u"FLOOD_PREMIUM_WAIT_"_q)) {
			const auto seconds = type.mid(type.lastIndexOf('_') + 1).toInt();
			state->timer.callOnce(std::max(
				kBackfillFloodPause,
				crl::time(seconds + 1) * 1000));
		} else {
			BackfillFinish(peer);
		}
	}).send();
}

} // namespace

bool Wanted(const QString &query) {
	for (const auto ch : query) {
		const auto code = ch.unicode();
		if ((code >= 0xAC00 && code <= 0xD7A3)
			|| (code >= 0x1100 && code <= 0x11FF)
			|| (code >= 0x3130 && code <= 0x318F)) {
			return true;
		}
	}
	return false;
}

bool Excluded(PeerId peer) {
	return AyuSettings::getInstance().isSearchIndexExcluded(DialogId(peer));
}

void Forget(PeerId peer) {
	Database::Instance().forget(int64(peer.value));
}

void IndexRaw(
		not_null<Main::Session*> session,
		PeerId peer,
		MsgId id,
		TimeId date,
		const QString &text) {
	if (text.isEmpty()
		|| !IsServerMsgId(id)
		|| Excluded(peer)) {
		return;
	}
	Database::Instance().queue({
		.account = int64(session->userId().bare),
		.peer = int64(peer.value),
		.id = int64(id.bare),
		.date = int64(date),
		.norm = Normalize(text).toUtf8(),
	});
}

void Index(not_null<HistoryItem*> item) {
	// Self-destructing messages are meant to disappear; don't keep them.
	if (!item->isRegular()
		|| item->ttlDestroyAt()
		|| (item->media() && item->media()->ttlSeconds())) {
		return;
	}
	IndexRaw(
		&item->history()->session(),
		item->history()->peer->id,
		item->id,
		item->date(),
		item->originalText().text);
}

void Search(
		not_null<Main::Session*> session,
		const QString &query,
		PeerId inPeer,
		int limit,
		Fn<void(std::vector<Result>)> done) {
	auto &database = Database::Instance();
	if (!database.opened()) {
		done({});
		return;
	}
	database.flushNow();
	const auto account = int64(session->userId().bare);
	const auto words = TextUtilities::PrepareSearchWords(query);
	const auto peer = int64(inPeer.value);
	const auto path = Database::Path();
	crl::async([=] {
		sqlite3 *db = nullptr;
		const auto flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX;
		auto result = std::vector<Result>();
		if (sqlite3_open_v2(path.constData(), &db, flags, nullptr)
			== SQLITE_OK) {
			result = RunSearch(db, account, words, peer, limit);
		}
		sqlite3_close(db);
		crl::on_main([=, result = std::move(result)]() mutable {
			done(std::move(result));
		});
	});
}

void ContinueManyAfter(not_null<PeerData*> peer);

void Backfill(
		not_null<PeerData*> peer,
		Fn<void(int)> progress,
		Fn<void()> done) {
	if (Backfills.contains(peer) || Excluded(peer->id)) {
		return;
	}
	// The session (and the peer with it) can go away mid-download.
	peer->session().lifetime().add([=] { Backfills.erase(peer); });
	const auto saved = Database::Instance().progress(
		int64(peer->session().userId().bare),
		int64(peer->id.value));
	if (saved.done) {
		// Already complete; newer messages get indexed as they arrive.
		crl::on_main(&peer->session(), [=] {
			if (done) {
				done();
			}
		});
		return;
	}
	auto state = std::make_unique<BackfillState>(peer);
	state->offsetId = MsgId(saved.offsetId);
	state->progress = std::move(progress);
	state->done = std::move(done);
	state->timer.setCallback([=] { BackfillNext(peer); });
	Backfills.emplace(peer, std::move(state));
	BackfillNext(peer);
}

bool BackfillRunning(not_null<PeerData*> peer) {
	return Backfills.contains(peer);
}

void CancelBackfill(not_null<PeerData*> peer) {
	const auto i = Backfills.find(peer);
	if (i == Backfills.end()) {
		return;
	}
	if (i->second->requestId) {
		peer->session().api().request(i->second->requestId).cancel();
	}
	Backfills.erase(i);
	ContinueManyAfter(peer); // A folder download moves on to the next chat.
}

namespace {

struct ManyState {
	not_null<Main::Session*> session;
	std::vector<not_null<PeerData*>> left;
	PeerData *current = nullptr;
	int messages = 0;
	int currentCount = 0;
	Fn<void(int)> done;
};

std::unique_ptr<ManyState> Many;

void BackfillManyNext() {
	if (!Many) {
		return;
	}
	Many->messages += base::take(Many->currentCount);
	Many->current = nullptr;
	while (!Many->left.empty()) {
		const auto peer = Many->left.front();
		Many->left.erase(Many->left.begin());
		if (Excluded(peer->id) || BackfillRunning(peer)) {
			continue;
		}
		Many->current = peer;
		Backfill(peer, [](int indexed) {
			if (Many) {
				Many->currentCount = indexed;
			}
		}, [] {
			BackfillManyNext();
		});
		return;
	}
	const auto state = std::move(Many);
	if (state->done) {
		state->done(state->messages);
	}
}

} // namespace

void ContinueManyAfter(not_null<PeerData*> peer) {
	if (Many && Many->current == peer.get()) {
		BackfillManyNext();
	}
}

void BackfillMany(
		not_null<Main::Session*> session,
		std::vector<not_null<PeerData*>> peers,
		Fn<void(int)> done) {
	if (Many) {
		return;
	}
	Many = std::make_unique<ManyState>(ManyState{
		.session = session,
		.left = std::move(peers),
		.done = std::move(done),
	});
	session->lifetime().add([=] {
		if (Many && Many->session == session) {
			Many = nullptr;
		}
	});
	BackfillManyNext();
}

bool BackfillManyRunning() {
	return Many != nullptr;
}

void CancelBackfillMany() {
	if (const auto state = base::take(Many)) {
		if (const auto peer = state->current) {
			CancelBackfill(peer);
		}
	}
}

} // namespace AyuLocalSearch
