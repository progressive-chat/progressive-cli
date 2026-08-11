// gomuks - A Matrix client written in Go.
// Copyright (C) 2024 Tulir Asokan
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
import { JSX, MouseEvent } from "react"
import { EventID, PinnedEventsContent } from "@/api/types"
import { RoomContextData, useRoomContext } from "@/ui/roomview/roomcontext.ts"
import { jumpToEventInView } from "@/ui/util/jumpToEvent.tsx"
import { listDiff } from "@/util/diff.ts"
import { humanJoinReact } from "@/util/reactjoin.tsx"
import { ensureTypedArray, getDisplayname, isEventID } from "@/util/validation.ts"
import EventContentProps from "./props.ts"

function renderPinChanges(
	roomCtx: RoomContextData,
	content: PinnedEventsContent,
	prevContent?: PinnedEventsContent,
): JSX.Element {
	const [added, removed] = listDiff(
		ensureTypedArray(content.pinned ?? [], isEventID),
		ensureTypedArray(prevContent?.pinned ?? [], isEventID),
	)
	const jumpToOnClick = (event_id: EventID) => (evt: MouseEvent<HTMLAnchorElement>) => {
		evt.preventDefault()
		jumpToEventInView(roomCtx, event_id, evt.currentTarget.closest(".timeline-list"))
	}
	const makeEventURI = (e: EventID) =>
		`matrix:roomid/${encodeURIComponent(roomCtx.store.roomID.slice(1))}/e/${encodeURIComponent(e.slice(1))}`

	const renderEventLink = (event_id: EventID) =>
		<a key={event_id} href={makeEventURI(event_id)} onClick={jumpToOnClick(event_id)}>
			{event_id}
		</a>

	if (added.length) {
		if (removed.length) {
			return <>
				pinned {humanJoinReact(added.map(renderEventLink))} and
				unpinned {humanJoinReact(removed.map(renderEventLink))}
			</>
		}
		return <>pinned {humanJoinReact(added.map(renderEventLink))}</>
	} else if (removed.length) {
		const removedLinks = removed.map(renderEventLink)
		return <>unpinned {humanJoinReact(removedLinks)}</>
	} else {
		return <>sent a no-op pin event</>
	}
}

const PinnedEventsBody = ({ event, sender }: EventContentProps) => {
	const roomCtx = useRoomContext()
	const content = event.content as PinnedEventsContent
	const prevContent = event.unsigned.prev_content as PinnedEventsContent | undefined
	return <div className="pinned-events-body">
		{getDisplayname(event.sender, sender?.content)} {renderPinChanges(roomCtx, content, prevContent)}
	</div>
}

export default PinnedEventsBody
