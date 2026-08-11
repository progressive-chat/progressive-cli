// gomuks - A Matrix client written in Go.
// Copyright (C) 2025 Tulir Asokan
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
import { use, useCallback, useEffect, useState } from "react"
import { getRoomAvatarThumbnailURL } from "@/api/media.ts"
import { RoomStateStore, useRoomState, useSpaceEdges } from "@/api/statestore"
import { DBSpaceEdge, MemDBEvent, RoomID, SpaceHierarchyChild } from "@/api/types"
import { useEventAsState } from "@/util/eventdispatcher.ts"
import { getEventLevel } from "@/util/powerlevel.ts"
import { ensureStringArray } from "@/util/validation.ts"
import ClientContext from "../ClientContext.ts"
import MainScreenContext from "../MainScreenContext.ts"
import { useFilteredRooms } from "../composer/userautocomplete.ts"
import { getPowerLevels } from "../menu/util.ts"
import { useRoomContext } from "./roomcontext.ts"
import AddIcon from "@/icons/add.svg?react"
import DeleteIcon from "@/icons/delete.svg?react"
import RecommendIcon from "@/icons/recommend.svg?react"
import VerifiedIcon from "@/icons/verified.svg?react"
import "./SpaceView.css"

interface SpaceChildProps {
	spaceID: RoomID
	roomID: RoomID
	edge?: DBSpaceEdge
	childEvt?: MemDBEvent
	summary?: SpaceHierarchyChild
	canModify: boolean
	store?: RoomStateStore
	onAdd?: () => void
}

const SpaceChild = ({
	spaceID, roomID, edge, summary, childEvt, canModify, store, onAdd,
}: SpaceChildProps) => {
	const mainScreen = use(MainScreenContext)
	const client = use(ClientContext)!
	store = store ?? client.store.rooms.get(roomID)
	const room = useEventAsState(store?.meta)
	const name = room?.name ?? summary?.name
	const onClickDelete = () => {
		if (!edge) {
			return
		}
		let confirmMessage: string
		if (edge.child_event_rowid) {
			if (edge.parent_event_rowid) {
				confirmMessage = `Remove both m.space.child and m.space.parent events of ${name} / ${roomID}?`
			} else {
				confirmMessage = `Remove m.space.child event of ${name} / ${roomID}?`
			}
		} else if (edge.parent_event_rowid) {
			confirmMessage = `Remove m.space.parent event in ${name} / ${roomID}?`
		} else {
			window.alert("No child or parent event rowid found 🤔")
			return
		}
		if (!window.confirm(confirmMessage)) {
			return
		}
		if (edge.child_event_rowid) {
			client.rpc.setState(spaceID, "m.space.child", roomID, {}).then(
				resp => console.info("Removed m.space.child", spaceID, "->", roomID, resp),
				err => {
					console.error("Failed to remove m.space.child", spaceID, "->", roomID, err)
					window.alert(`Failed to remove m.space.child event: ${err}`)
				},
			)
		}
		if (edge.parent_event_rowid) {
			client.rpc.setState(roomID, "m.space.parent", spaceID, {}).then(
				resp => console.info("Removed m.space.parent", roomID, "->", spaceID, resp),
				err => {
					console.error("Failed to remove m.space.parent", roomID, "->", spaceID, err)
					window.alert(`Failed to remove m.space.parent event: ${err}`)
				},
			)
		}
	}
	const onClickAdd = () => {
		client.rpc.setState(spaceID, "m.space.child", roomID, { via: store!.getViaServers() }).then(
			resp => console.info("Added m.space.child", spaceID, "->", roomID, resp),
			err => {
				console.error("Failed to add m.space.child", spaceID, "->", roomID, err)
				window.alert(`Failed to add m.space.child event: ${err}`)
			},
		).finally(onAdd)
	}
	const joinRoom = () => {
		mainScreen.setActiveRoom(roomID, {
			previewMeta: {
				roomID: roomID,
				via: ensureStringArray(childEvt?.content.via),
			},
		})
	}
	const isSpace = (room?.creation_content?.type ?? summary?.room_type) === "m.space"
	return <>
		<div
			className={`space-child ${room ? "known-room" : "unknown-room"} ${edge ? "existing-edge" : ""}`}
			onClick={edge ? joinRoom : undefined}
		>
			<img
				src={getRoomAvatarThumbnailURL(room ?? summary ?? { room_id: roomID })}
				loading="lazy"
				alt=""
				className={`avatar ${isSpace ? "space" : ""}`}
			/>
			<div className="room-id-and-name">
				{name !== undefined ? <span className="room-name">{name}</span> : null}
				<span className="room-id">{roomID}</span>
			</div>
		</div>
		{edge ? <div className="buttons">
			{edge.canonical && <button disabled title="This is the canonical parent space"><VerifiedIcon /></button>}
			{edge.suggested && <button disabled title="Suggested room in space"><RecommendIcon /></button>}
			{canModify && <button onClick={onClickDelete}><DeleteIcon /></button>}
		</div> : <div className="buttons">
			<button onClick={onClickAdd}><AddIcon /></button>
		</div>}
	</>
}

const SpaceAdder = () => {
	const roomCtx = useRoomContext()
	const client = use(ClientContext)!
	const [query, setQuery] = useState("")
	const clearQuery = useCallback(() => setQuery(""), [])
	const filteredRooms = useFilteredRooms(client.store, query)
	return <div className="space-adder">
		<input
			type="text"
			value={query}
			onChange={e => setQuery(e.target.value)}
			placeholder="Search rooms to add..."
		/>
		<div className="space-children">
			{filteredRooms.map(room => {
				const existingChild = roomCtx.store.getStateEvent("m.space.child", room.roomID)
				if (existingChild && Array.isArray(existingChild.content.via)) {
					return null
				}
				return <SpaceChild
					spaceID={roomCtx.store.roomID} roomID={room.roomID} canModify={true} store={room} onAdd={clearQuery}
				/>
			})}
		</div>
	</div>
}

const emptyMap = new Map<RoomID, SpaceHierarchyChild>()

const SpaceView = () => {
	const [hierarchy, setHierarchy] = useState<Map<RoomID, SpaceHierarchyChild>>(emptyMap)
	const roomCtx = useRoomContext()
	const client = use(ClientContext)!
	const edgeStore = client.store.spaceEdges.get(roomCtx.store.roomID)
	const children = useSpaceEdges(edgeStore)
	useRoomState(roomCtx.store, "m.room.power_levels", "")
	useEffect(() => {
		let cancelled = false
		client.rpc.getSpaceHierarchy(roomCtx.store.roomID, {
			limit: 50,
			max_depth: 1,
		}).then(hier => {
			if (!cancelled) {
				const hierarchyMap = new Map(hier.rooms.map(item => [item.room_id, item]))
				console.debug("Fetched hierarchy", hierarchyMap)
				setHierarchy(hierarchyMap)
			}
		}, err => {
			console.error("Failed to fetch space hierarchy:", err)
			// TODO display error?
		})
		return () => {
			cancelled = true
		}
	}, [client, roomCtx.store.roomID])
	if (!children) {
		return "not a space? :thinking:"
	}
	const [pls, ownPL] = getPowerLevels(roomCtx.store, client)
	const canModifySpace = getEventLevel(pls, "m.space.child", true) <= ownPL
	// TODO display hidden space rooms (only parent rowid set)
	return <div className="space-view">
		{canModifySpace && <SpaceAdder />}
		<div className="space-children">
			{children.map(edge => edge.child_event_rowid /*|| edge.parent_event_rowid*/ ? <SpaceChild
				spaceID={roomCtx.store.roomID}
				roomID={edge.child_id}
				childEvt={edge.child_event_rowid ? roomCtx.store.eventsByRowID.get(edge.child_event_rowid) : undefined}
				edge={edge}
				summary={hierarchy.get(edge.child_id)}
				canModify={canModifySpace}
				key={edge.child_id}
			/> : null)}
		</div>
	</div>
}

export default SpaceView
