#!/usr/bin/env python3
"""A minimal StatusNotifierItem, for testing edwm's SNI host.

Publishes an item, registers with the watcher, and then exercises the parts a
host has to get right:

  * IconName changes, announced with NewIcon
  * Status flipping between Active and Passive, announced with NewStatus
  * Activate / SecondaryActivate / ContextMenu, which it logs

    ./snitest.py [icon-name] [--cycle]

--cycle alternates icon and status every few seconds so a host can be watched
handling updates rather than just the initial fetch. --passive starts hidden,
which a host must honour by drawing nothing at all.
"""
import sys
import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

ITEM_IFACE = 'org.kde.StatusNotifierItem'
PROPS_IFACE = 'org.freedesktop.DBus.Properties'

ICONS = ['audio-volume-high', 'audio-volume-muted']


class Item(dbus.service.Object):
    def __init__(self, bus, path, icon):
        super().__init__(bus, path)
        self.icon = icon
        self.status = 'Passive' if '--passive' in sys.argv else 'Active'

    def props(self):
        return {
            'Category': 'ApplicationStatus',
            'Id': 'snitest',
            'Title': 'snitest',
            'Status': self.status,
            'IconName': self.icon,
            'IconThemePath': '',
            'ItemIsMenu': False,
        }

    @dbus.service.method(PROPS_IFACE, in_signature='ss', out_signature='v')
    def Get(self, iface, prop):
        return self.props().get(prop, '')

    @dbus.service.method(PROPS_IFACE, in_signature='s', out_signature='a{sv}')
    def GetAll(self, iface):
        return self.props()

    @dbus.service.method(ITEM_IFACE, in_signature='ii')
    def Activate(self, x, y):
        print(f'ACTIVATE at {x},{y}', flush=True)

    @dbus.service.method(ITEM_IFACE, in_signature='ii')
    def SecondaryActivate(self, x, y):
        print(f'SECONDARY at {x},{y}', flush=True)

    @dbus.service.method(ITEM_IFACE, in_signature='ii')
    def ContextMenu(self, x, y):
        print(f'CONTEXTMENU at {x},{y}', flush=True)

    @dbus.service.signal(ITEM_IFACE)
    def NewIcon(self):
        pass

    @dbus.service.signal(ITEM_IFACE, signature='s')
    def NewStatus(self, status):
        pass

    def cycle(self):
        self.icon = ICONS[(ICONS.index(self.icon) + 1) % len(ICONS)] \
            if self.icon in ICONS else ICONS[0]
        print(f'icon -> {self.icon}', flush=True)
        self.NewIcon()
        return True

    def toggle_status(self):
        self.status = 'Passive' if self.status == 'Active' else 'Active'
        print(f'status -> {self.status}', flush=True)
        self.NewStatus(self.status)
        return True


def main():
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    icon = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-') \
        else ICONS[0]
    path = '/StatusNotifierItem'
    item = Item(bus, path, icon)

    watcher = dbus.Interface(
        bus.get_object('org.kde.StatusNotifierWatcher', '/StatusNotifierWatcher'),
        'org.kde.StatusNotifierWatcher')
    # Register by object path, the form libappindicator uses: the host has to
    # fall back to our unique bus name.
    watcher.RegisterStatusNotifierItem(path)
    print(f'registered {bus.get_unique_name()}{path} icon={icon}', flush=True)

    if '--cycle' in sys.argv:
        GLib.timeout_add_seconds(4, item.cycle)
        GLib.timeout_add_seconds(10, item.toggle_status)

    GLib.MainLoop().run()


main()
