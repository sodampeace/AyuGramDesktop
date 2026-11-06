// See chat_exclusions.h.
#include "ayu/features/exclusions/chat_exclusions.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/local_search/local_search.h"
#include "ayu/utils/telegram_helpers.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

namespace AyuExclusions {
namespace {

[[nodiscard]] const std::unordered_set<int64> &Ids(Kind kind) {
	const auto &settings = AyuSettings::getInstance();
	return (kind == Kind::DeletedMessages)
		? settings.deletedSaveExcludedIds()
		: settings.searchIndexExcludedIds();
}

void Store(Kind kind, std::unordered_set<int64> ids) {
	auto &settings = AyuSettings::getInstance();
	if (kind == Kind::DeletedMessages) {
		settings.setDeletedSaveExcludedIds(std::move(ids));
	} else {
		settings.setSearchIndexExcludedIds(std::move(ids));
	}
}

// Drops what is already indexed for a chat that just got excluded.
void ForgetIndexed(not_null<PeerData*> peer) {
	AyuLocalSearch::CancelBackfill(peer);
	AyuLocalSearch::Forget(peer->id);
}

[[nodiscard]] PeerData *Resolve(not_null<Main::Session*> session, int64 id) {
	auto &data = session->data();
	if (id > 0) {
		return data.userLoaded(UserId(id));
	} else if (const auto channel = data.channelLoaded(ChannelId(-id))) {
		return channel;
	}
	return data.chatLoaded(ChatId(-id));
}

class Controller final : public ChatsListBoxController {
public:
	Controller(
		not_null<Main::Session*> session,
		rpl::producer<QString> title,
		std::vector<not_null<PeerData*>> selected)
	: ChatsListBoxController(session)
	, _session(session)
	, _title(std::move(title))
	, _selected(std::move(selected)) {
	}

	Main::Session &session() const override {
		return *_session;
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		delegate()->peerListSetRowChecked(row, !row->checked());
	}

private:
	void prepareViewHook() override {
		delegate()->peerListSetTitle(std::move(_title));
		delegate()->peerListAddSelectedPeers(_selected);
	}

	std::unique_ptr<Row> createRow(not_null<History*> history) override {
		return std::make_unique<Row>(history);
	}

	const not_null<Main::Session*> _session;
	rpl::producer<QString> _title;
	std::vector<not_null<PeerData*>> _selected;
};

} // namespace

bool IsExcluded(Kind kind, not_null<PeerData*> peer) {
	return Ids(kind).contains(getDialogIdFromPeer(peer));
}

void SetExcluded(Kind kind, not_null<PeerData*> peer, bool excluded) {
	auto ids = Ids(kind);
	const auto id = getDialogIdFromPeer(peer);
	if (excluded) {
		ids.insert(id);
	} else {
		ids.erase(id);
	}
	Store(kind, std::move(ids));
	if (excluded && kind == Kind::SearchIndex) {
		ForgetIndexed(peer);
	}
}

void ShowBox(not_null<Window::SessionController*> controller, Kind kind) {
	const auto session = &controller->session();

	// Ids of chats this account does not know stay as they are on save.
	auto selected = std::vector<not_null<PeerData*>>();
	auto unknown = std::unordered_set<int64>();
	for (const auto id : Ids(kind)) {
		if (const auto peer = Resolve(session, id)) {
			selected.push_back(peer);
		} else {
			unknown.insert(id);
		}
	}

	auto title = (kind == Kind::DeletedMessages)
		? tr::ayu_ExcludedDeletedTitle()
		: tr::ayu_ExcludedSearchTitle();
	auto list = std::make_unique<Controller>(
		session,
		std::move(title),
		std::move(selected));
	auto init = [=](not_null<PeerListBox*> box) {
		box->addButton(tr::lng_settings_save(), [=] {
			auto ids = unknown;
			const auto before = Ids(kind);
			const auto peers = box->collectSelectedRows();
			for (const auto &peer : peers) {
				ids.insert(getDialogIdFromPeer(peer));
			}
			Store(kind, std::move(ids));
			if (kind == Kind::SearchIndex) {
				for (const auto &peer : peers) {
					if (!before.contains(getDialogIdFromPeer(peer))) {
						ForgetIndexed(peer);
					}
				}
			}
			box->closeBox();
			controller->showToast(tr::ayu_ExcludedSaved(tr::now));
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	};
	controller->show(Box<PeerListBox>(std::move(list), std::move(init)));
}

} // namespace AyuExclusions
