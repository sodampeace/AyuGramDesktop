// Mirrors the progress messages that the `eta` bot keeps editing in one chat
// into ~/Library/Application Support/EtaBar/tasks.json. The menu bar app
// (EtaBar) reads that file; this side only collects raw message text.
#include "ayu/features/eta/eta_watcher.h"

#include "apiwrap.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_domain.h"
#include "main/main_session.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>

namespace AyuEta {
namespace {

constexpr auto kRecentLimit = 30;
constexpr auto kRefreshInterval = 30 * crl::time(1000);

struct Task {
	QString text;
	TimeId date = 0;
	TimeId seen = 0;
};

// Bot API chat ids: -100xxxxxxxxxx is a supergroup, -xxxxxxxxxx a basic group.
[[nodiscard]] PeerId ReadEtaChat() {
	QFile file(QDir::homePath() + u"/.config/eta/config.json"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return PeerId();
	}
	const auto id = QJsonDocument::fromJson(file.readAll())
		.object()
		.value(u"chat_id"_q)
		.toVariant()
		.toLongLong();
	constexpr auto kChannelShift = 1000000000000LL;
	if (id <= -kChannelShift) {
		return peerFromChannel(ChannelId(-id - kChannelShift));
	} else if (id < 0) {
		return peerFromChat(ChatId(-id));
	}
	return PeerId();
}

[[nodiscard]] QString OutputPath() {
	return QStandardPaths::writableLocation(
		QStandardPaths::GenericDataLocation) + u"/EtaBar/tasks.json"_q;
}

// Replies may come after an account switch destroyed us; guard them.
class Watcher final : public base::has_weak_ptr {
public:
	Watcher(not_null<Main::Session*> session, PeerId chat);

private:
	[[nodiscard]] bool isEtaItem(not_null<const HistoryItem*> item) const;
	void upsert(not_null<HistoryItem*> item);
	void remove(MsgId id);
	void requestRecent();
	void write();

	const not_null<Main::Session*> _session;
	const PeerId _chat;
	base::flat_map<MsgId, Task> _tasks;
	base::Timer _refresh;
	mtpRequestId _requestId = 0;
	rpl::lifetime _lifetime;
};

Watcher::Watcher(not_null<Main::Session*> session, PeerId chat)
: _session(session)
, _chat(chat)
, _refresh([=] { requestRecent(); }) {
	const auto data = &session->data();
	data->newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		if (isEtaItem(item)) upsert(item);
	}, _lifetime);

	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		if (isEtaItem(update.item)) upsert(update.item);
	}, _lifetime);

	data->itemRemoved(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		if (item->history()->peer->id == _chat) remove(item->id);
	}, _lifetime);

	write();
	requestRecent();
}

bool Watcher::isEtaItem(not_null<const HistoryItem*> item) const {
	if (item->history()->peer->id != _chat || !item->isRegular()) {
		return false;
	}
	const auto from = item->from();
	return from->isUser() && from->asUser()->isBot();
}

// eta starts a running task with ⏳ (or ⏸ while paused) and edits it to
// ✅ / ❌ when it ends. Only running ones are tasks; results and questions
// the bot leaves in the chat are not.
[[nodiscard]] bool IsRunning(const QString &text) {
	return text.startsWith(QString::fromUtf8("\u23F3"))
		|| text.startsWith(QString::fromUtf8("\u23F8"));
}

void Watcher::upsert(not_null<HistoryItem*> item) {
	if (!IsRunning(item->originalText().text)) {
		remove(item->id);
		return;
	}
	auto &task = _tasks[item->id];
	task.text = item->originalText().text;
	task.date = item->date();
	task.seen = base::unixtime::now();
	write();
	if (!_refresh.isActive()) {
		_refresh.callEach(kRefreshInterval);
	}
}

void Watcher::remove(MsgId id) {
	if (_tasks.remove(id)) {
		write();
	}
	if (_tasks.empty()) {
		_refresh.cancel();
	}
}

// Fills in tasks that started before the app was launched and drops ones
// deleted while we were offline (eta deletes a message when its task ends).
void Watcher::requestRecent() {
	const auto peer = _session->data().peerLoaded(_chat);
	if (!peer) {
		// Chat list not loaded yet right after launch: try again later.
		if (!_refresh.isActive()) {
			_refresh.callEach(kRefreshInterval);
		}
		return;
	} else if (_requestId) {
		return;
	}
	_requestId = _session->api().request(MTPmessages_GetHistory(
		peer->input(),
		MTP_int(0), // offset_id
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(kRecentLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done(crl::guard(this, [=](const MTPmessages_Messages &result) {
		_requestId = 0;
		const auto data = &_session->data();
		data->processExistingMessages(peer->asChannel(), result);
		auto alive = base::flat_set<MsgId>();
		result.match([&](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &list) {
			for (const auto &message : list.vmessages().v) {
				const auto id = IdFromMessage(message);
				if (const auto item = data->message(_chat, id)) {
					if (isEtaItem(item)
						&& IsRunning(item->originalText().text)) {
						alive.emplace(id);
						upsert(item);
					}
				}
			}
		});
		for (auto i = _tasks.begin(); i != _tasks.end();) {
			i = alive.contains(i->first) ? (i + 1) : _tasks.erase(i);
		}
		if (_tasks.empty()) {
			_refresh.cancel();
		}
		write();
	})).fail(crl::guard(this, [=] {
		_requestId = 0;
	})).send();
}

void Watcher::write() {
	auto list = QJsonArray();
	for (const auto &[id, task] : _tasks) {
		list.push_back(QJsonObject{
			{ u"id"_q, qint64(id.bare) },
			{ u"text"_q, task.text },
			{ u"date"_q, qint64(task.date) },
			{ u"seen"_q, qint64(task.seen) },
		});
	}
	const auto path = OutputPath();
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSaveFile file(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{
			{ u"updated"_q, qint64(base::unixtime::now()) },
			{ u"tasks"_q, list },
		}).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

std::unique_ptr<Watcher> Current;
rpl::lifetime Lifetime;

} // namespace

void start() {
	const auto chat = ReadEtaChat();
	if (!chat) {
		return;
	}
	Core::App().domain().activeSessionValue(
	) | rpl::on_next([=](Main::Session *session) {
		Current = session
			? std::make_unique<Watcher>(session, chat)
			: nullptr;
	}, Lifetime);
}

} // namespace AyuEta
