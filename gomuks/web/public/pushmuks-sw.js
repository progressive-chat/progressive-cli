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
self.addEventListener("install", () => self.skipWaiting())
self.addEventListener("activate", (event) => event.waitUntil(clients.claim()))

self.addEventListener("push", evt => {
	if (self.Notification?.permission !== "granted") {
		return
	}
	const data = evt.data.json()
	evt.waitUntil(Promise.all(data.messages.map(notif => self.registration.showNotification(
		notif.room_name === notif.sender.name ? notif.sender.name : `${notif.sender.name} (${notif.room_name})`,
		{
			body: notif.text,
			timestamp: notif.timestamp,
			silent: !notif.sound,
			badge: "gomuks.png",
			icon: notif.sender.avatar ? `${notif.sender.avatar}&image_auth=${data.image_auth}` : undefined,
			image: notif.image ? `${notif.image}&image_auth=${data.image_auth}` : undefined,
			data: {
				url: `#/uri/${encodeURIComponent(`matrix:roomid/${notif.room_id.slice(1)}/e/${notif.event_id.slice(1)}`)}`
			},
		},
	))))
})

self.addEventListener("notificationclick", evt => {
	evt.notification.close()
	evt.waitUntil(self.clients.matchAll({ type: "window" }).then(async clientList => {
		const url = new URL(evt.notification.data.url, self.location.origin)
		for (const client of clientList) {
			try {
				await Promise.all(client.focus(), client.navigate(evt.notification.data.url))
				return
			} catch (err) {
				console.error("Error navigating client", client, err)
			}
		}
		self.clients.openWindow(url.href)
	}))
})
