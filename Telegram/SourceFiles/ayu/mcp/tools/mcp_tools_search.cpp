// MCP tools for the local Korean search index: start, pause and watch the
// history download of a chat folder without touching the UI.
#include "ayu/mcp/tools/mcp_tools.h"

#include "ayu/features/local_search/local_search.h"
#include "ayu/mcp/mcp_registry.h"
#include "data/data_chat_filters.h"
#include "data/data_session.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "main/main_session.h"

namespace AyuMcp {
namespace {

void listFolders(
		const CallContext &context,
		const QJsonObject &arguments,
		Fn<void(ToolResult)> done) {
	auto &filters = context.session->data().chatsFilters();
	auto folders = QJsonArray();
	auto lines = QStringList();
	for (const auto &filter : filters.list()) {
		if (!filter.id()) {
			continue;
		}
		const auto title = filter.title().text.text;
		const auto count = filters.chatsList(filter.id())->indexed()->size();
		folders.append(QJsonObject{
			{ u"id"_q, filter.id() },
			{ u"title"_q, title },
			{ u"chats"_q, count },
		});
		lines.append(u"%1 | %2 | %3 chats"_q.arg(filter.id()).arg(title).arg(count));
	}
	done({
		.text = lines.isEmpty() ? u"No folders."_q : lines.join('\n'),
		.structured = QJsonObject{ { u"folders"_q, folders } },
	});
}

[[nodiscard]] QJsonObject statusJson() {
	const auto status = AyuLocalSearch::BackfillManyStatus();
	return {
		{ u"running"_q, status.running },
		{ u"chats_left"_q, status.chatsLeft },
		{ u"current_chat"_q, status.currentChat },
		{ u"messages"_q, status.messages },
	};
}

[[nodiscard]] QString statusText() {
	const auto status = AyuLocalSearch::BackfillManyStatus();
	return status.running
		? u"Downloading \"%1\", %2 chats left, %3 messages saved this run."_q
			.arg(status.currentChat)
			.arg(status.chatsLeft)
			.arg(status.messages)
		: u"Not downloading."_q;
}

void startFolder(
		const CallContext &context,
		const QJsonObject &arguments,
		Fn<void(ToolResult)> done) {
	if (AyuLocalSearch::BackfillManyRunning()) {
		done({ .text = statusText(), .structured = statusJson() });
		return;
	}
	const auto id = FilterId(arguments.value(u"folder_id"_q).toInt());
	auto &filters = context.session->data().chatsFilters();
	if (!id || !ranges::contains(filters.list(), id, &Data::ChatFilter::id)) {
		done(toolError(u"Unknown folder_id. Call list_folders first."_q));
		return;
	}
	auto peers = std::vector<not_null<PeerData*>>();
	for (const auto &entry : *filters.chatsList(id)->indexed()) {
		if (const auto history = entry->history()) {
			peers.push_back(history->peer);
		}
	}
	// Chats finished earlier are skipped, half-done ones continue.
	AyuLocalSearch::BackfillMany(context.session, std::move(peers), nullptr);
	done({ .text = statusText(), .structured = statusJson() });
}

void pause(
		const CallContext &context,
		const QJsonObject &arguments,
		Fn<void(ToolResult)> done) {
	AyuLocalSearch::CancelBackfillMany();
	done({ .text = u"Paused. Starting the folder again resumes it."_q });
}

void status(
		const CallContext &context,
		const QJsonObject &arguments,
		Fn<void(ToolResult)> done) {
	done({ .text = statusText(), .structured = statusJson() });
}

} // namespace

void registerSearchTools() {
	const auto empty = parseSchema(R"({
		"type": "object", "properties": {}, "additionalProperties": false
	})");
	registerTool({
		.name = u"list_folders"_q,
		.title = u"List chat folders"_q,
		.description = u"Lists chat folders with their ids."_q,
		.inputSchema = empty,
		.handler = listFolders,
	});
	registerTool({
		.name = u"search_backfill_folder"_q,
		.title = u"Download folder history for search"_q,
		.description = u"Downloads the full history of every chat in a "
			"folder into the local search index, one chat at a time. "
			"Resumes where an earlier run stopped."_q,
		.inputSchema = parseSchema(R"({
			"type": "object",
			"properties": {
				"folder_id": { "type": "integer", "minimum": 1 }
			},
			"required": ["folder_id"],
			"additionalProperties": false
		})"),
		.hints = { .readOnly = false, .idempotent = true, .openWorld = true },
		.handler = startFolder,
	});
	registerTool({
		.name = u"search_backfill_pause"_q,
		.title = u"Pause history download"_q,
		.description = u"Pauses the folder history download."_q,
		.inputSchema = empty,
		.hints = { .readOnly = false },
		.handler = pause,
	});
	registerTool({
		.name = u"search_backfill_status"_q,
		.title = u"History download status"_q,
		.description = u"Shows whether a folder history download runs."_q,
		.inputSchema = empty,
		.handler = status,
	});
}

} // namespace AyuMcp
