// Local message index for Korean search. See local_search.h.
#include "ayu/features/local_search/local_search.h"

#include "ayu/libs/sqlite/sqlite3.h"
#include "apiwrap.h"
#include "ayu/ayu_settings.h"
#include "base/timer.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/text/text_entity.h"

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
		const auto sql = "DELETE FROM messages WHERE peer = ?";
		if (sqlite3_prepare_v2(_db, sql, -1, &statement, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(statement, 1, peer);
			sqlite3_step(statement);
			sqlite3_finalize(statement);
		}
	}

	std::vector<Result> search(
			int64 account,
			const QStringList &words,
			int64 inPeer,
			int limit) {
		flush();
		auto result = std::vector<Result>();
		if (!_db || words.isEmpty()) {
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
		if (sqlite3_prepare_v2(_db, sql.constData(), -1, &statement, nullptr)
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

private:
	Database() : _flush([=] { flush(); }) {
		const auto path = (cWorkingDir() + u"tdata/ayu_search.db"_q).toUtf8();
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
		if (!count || !lowest || (state->offsetId && lowest >= state->offsetId)) {
			BackfillFinish(peer);
			return;
		}
		state->offsetId = lowest;
		state->timer.callOnce(kBackfillDelay);
	}).fail([=](const MTP::Error &error) {
		state->requestId = 0;
		// FLOOD_WAIT and friends: wait and try the same page again.
		if (error.type().startsWith(u"FLOOD_WAIT_"_q)) {
			const auto seconds = error.type().mid(11).toInt();
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
	if (!item->isRegular()) {
		return;
	}
	IndexRaw(
		&item->history()->session(),
		item->history()->peer->id,
		item->id,
		item->date(),
		item->originalText().text);
}

std::vector<Result> Search(
		not_null<Main::Session*> session,
		const QString &query,
		PeerId inPeer,
		int limit) {
	return Database::Instance().search(
		int64(session->userId().bare),
		TextUtilities::PrepareSearchWords(query),
		int64(inPeer.value),
		limit);
}

void Backfill(
		not_null<PeerData*> peer,
		Fn<void(int)> progress,
		Fn<void()> done) {
	if (Backfills.contains(peer)) {
		return;
	}
	auto state = std::make_unique<BackfillState>(peer);
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
}

} // namespace AyuLocalSearch
