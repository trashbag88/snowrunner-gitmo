===============================================================================
  Gitmo for SnowRunner  v0.9.12 (transfers, text messages, photos and radio)
===============================================================================

WHAT IT IS
----------
A phone in your game. Press Delete to open it: a home screen with apps.
The phone works anywhere: in the game's main menu, and with a save loaded
(single player or multiplayer, driving or in the garage). Sending and
receiving money needs a loaded save; everything else works in the menu too.
The Gitmo app shows:

  - Your balance, read live from the game, for whichever of your four
    save slots is loaded ("save 2" on the balance card).
  - Activity: every time your money goes up or down, with day, time and
    new balance, kept per save slot and from earlier sessions too (the
    last 300 per save, in Gitmo-activity.txt). Transfers show who they were to or from;
    transfers that moved no money (declined, no answer, canceled) are
    listed too, crossed out.
  - Send: give money to another player who has Gitmo (see TRANSFERS).
  - Light and dark themes, wallpapers, a battery, a cracked-screen look.
    A speaker in the phone's status bar shows whether the notification
    sound is on or muted.

The SMS app: text messages with other players who have Gitmo (see SMS).
The Radio app: internet radio on the phone (see RADIO).

Request and Bank are on the phone but not working yet.

Works on a flat screen and, with the SnowRunner VR mod, as a panel in the
headset (Settings > VR).


TRANSFERS
---------
There is no bank in the middle. Every player keeps their own game money;
Gitmo only moves an amount from one player's balance to another's, and
only after the receiver says yes.

  Who you can send to:  players in the same game session as you (co-op),
                        if they have Gitmo too. Gitmo asks the game itself
                        who is in your session. Someone who leaves your
                        game drops off the list within a few seconds, and
                        money offered by someone outside your game is
                        turned down by itself. Works with Steam friends;
                        players who aren't on your friends list may not
                        show up. Epic / Xbox players can't be reached.
                        SessionOnly=0 in Gitmo.ini instead lists every Steam
                        friend playing SnowRunner who has Gitmo.

  Sending:   Gitmo > Send > pick the player > type the amount > Send.
             Their phone asks "<you> wants to send you $N" with Accept and
             Decline. Nothing leaves your balance until they accept.
             They have 60 seconds to answer. Cancel works until then; if
             you cancel, their phone plays its notification sound and says
             "Transfer canceled".

  Receiving: the phone plays its notification sound (turn it off in
             Settings > Notifications). Open the phone (Delete) and the
             request is on top, whatever app is open. A badge on the
             Gitmo icon shows how many are waiting.
             To use your own sound, put a file called Gitmo-notify.wav
             next to version.dll.

  Both players need to be in a map (Gitmo must be sure which balance the
  game uses). If Gitmo isn't sure yet, earn or spend any money first.

  If something goes wrong: Gitmo never adds money the receiver didn't
  accept, and never adds the same transfer twice. If the connection drops
  or a game closes after the money left your balance but before it
  arrived, the transfer is cancelled and the money comes back to you, as
  soon as both of you are online again. Unsettled payments are kept in
  Gitmo-transfers.txt so they survive a restart.

  The game saves money itself. If a game crashes before it saves, that
  player's balance goes back to their last save, as with any other money
  change.

  Limits: up to $100,000,000 per transfer; one transfer waiting for an
  answer at a time.


SMS
---
Text other players who have Gitmo, like a phone's messages app.

  Contacts:  everyone with Gitmo who is online now (Steam friends), with
             their Steam picture and a green dot, plus everyone you've
             written with before, shown faded while they're offline.
             Conversations are kept (the last 500 messages per contact,
             in Gitmo-sms.txt next to the game), also over restarts and
             updates. Settings > Messages > Erase deletes them all from this PC
             (it asks first; the other players keep their copies).
  Writing:   open a contact, type, press Enter or the blue arrow. While
             you're typing, the menu key types instead of closing the
             phone: click outside the text field first.
  Delivery:  there's no server. A message to someone who is offline waits
             on your PC ("Waiting - sends when they're online") and goes
             out as soon as you're both online. "Delivered" and "Read"
             show under your last message.
  Incoming:  the notification sound plays and the SMS icon shows how many
             are unread.
  Emoji:     the smiley at the left of the text field opens an emoji panel:
             scroll through it or jump with the tabs along the bottom; the
             ones you use most recently come first. Tap the keyboard icon or
             the text field to go back to typing. Emoji show in colour.
  Photos:    the camera at the left of the text field. Take photo grabs
             the game's view as it is right now - set up the camera (cab or
             outside), open the phone, tap it; the phone isn't in the picture.
             It's also kept at full size: on Steam, in your Steam screenshots
             for SnowRunner (the same as an F12 screenshot); on Epic or any
             other store, in your Pictures\Gitmo folder (SavePhotos=0 stops it).
             Or pick an earlier one from the panel (on Steam: your F12
             screenshots; elsewhere: the Pictures\Gitmo photos). You see it before
             it's sent; tap a photo in a chat to see it big. Your chats' photos
             (sent and received) are kept in the Gitmo-photos folder next to
             the game (Erase removes them).
             Photos need Gitmo 0.8.0 or newer on both sides.
  Both players need Gitmo 0.7.0 or newer.

RADIO
-----
Internet radio, played through your PC's speakers or headphones (in VR it
comes through the headset like the game's sound). Only you hear it; your
friends pick their own station.

  - My stations: your list. Tap a station to play it, tap it again (or the
    player at the bottom) for Now playing. Add station takes a stream link
    (http:// or https://, a .pls or .m3u link works too); Test play checks
    it and fills in the name. Edit lets you rename, change or delete one.
  - Browse: search the free radio-browser.info directory by name or genre
    (it opens with the most popular stations). The star saves one.
  - Now playing: song and artist when the station sends them, stop/play,
    save, Share (texts what you're listening to and the station's link),
    and the radio's own volume (the game's volume isn't touched).
  - A station shared with you shows in SMS with its link underlined: tap
    it and Radio plays it (Now playing; the star saves it).
  - It keeps playing with the phone put away; a note in the status bar and
    a player on the home screen show it's on.
    It keeps playing through map changes and in the main menu. If the
    phone's battery goes flat, the radio stops.
  - Stations work if they stream MP3 or AAC (most do). HLS, Ogg and FLAC
    streams aren't supported; the phone says so.
  - Your stations and volume are kept in Gitmo-radio.ini (one line each:
    Name | link | genre), which you can also edit with the game closed.


INSTALL
-------
1. Close SnowRunner.
2. Unzip this folder anywhere (not straight into the game folder).
3. Double-click "Install Gitmo.bat". It finds SnowRunner in your Steam
   libraries, copies Gitmo in, and keeps your settings when updating.
   If it can't find the game, it asks you to paste the Bin folder.
   If Windows shows "Windows protected your PC", click More info >
   Run anyway.

Updating to a new version: same steps; the old version is kept as
version.dll.gitmo-previous.

By hand instead: copy version.dll and Gitmo.ini into SnowRunner's Bin
folder (the folder that contains SnowRunner.exe):

  Steam: ...\steamapps\common\SnowRunner\Sources\Bin\

If that folder already has a version.dll from another mod, Gitmo can't be
installed next to it (the installer checks this and changes nothing).

Gitmo is standalone. It uses its own file name (version.dll), so it sits
alongside NoAutoReverse (dinput8.dll) and the VR mod (dxgi.dll) without
replacing either. It uses the game's own Steam connection, on a channel of
its own, so it doesn't touch the game's multiplayer traffic.


USING IT
--------
  Delete             open / close the phone (change it on the phone:
                     Settings > Controls > Change)
  Mouse              tap buttons, switch tabs
  Phone border       click and drag the black border to move the phone;
                     it stays where you leave it
  While open         the game doesn't get your mouse or keyboard (clicks on
                     the phone never reach the game). Wheel, pedals and
                     shifter keep working.

When the game starts, Gitmo reads your money from your newest save, then
finds that value in the game's memory (about 20 seconds after start, and
again whenever it loses track). The card on the home screen shows "Live"
once it's found.


SETTINGS  (Gitmo.ini)
---------------------
  Enabled=1        0 = Gitmo loads but does nothing.
  MenuKey=Delete   Delete, Insert, Home, End, PageUp, PageDown, F1 - F12,
                   a letter or digit. Also changeable on the phone.
  Theme=light      light or dark (also switchable on the phone).
  Transfers=1      0 = no transfers (Gitmo doesn't contact anyone).
  NotifyMode=sound    0 = no sound when someone sends you money.
  SessionOnly=1    1 = transfers only with players in your game session;
                   0 = with any Steam friend playing SnowRunner.
  Display=auto     auto, screen or headset: where the phone shows.
  VrDistance=0.60  VR: metres from your head to the phone.
  VrPointerSpeed=2.0  VR: mouse pointer speed on the phone.
  SavePhotos=1     keep photos taken with the phone: on Steam in your Steam
                   screenshots, otherwise in Pictures\Gitmo. 0 = don't.
  Log=1            write Gitmo.log (troubleshooting).
  Research=0       developer research logging; leave off.


UNINSTALL
---------
Close the game and double-click "Uninstall Gitmo.bat" (it warns you first
if a transfer you sent is still settling). By hand: delete version.dll, Gitmo.ini, Gitmo.log,
Gitmo.previous.log, Gitmo-activity.txt, Gitmo-sms.txt, Gitmo-transfers.txt and the Gitmo-photos folder from the Bin
folder. Gitmo never changes game files or
saves directly. (Before deleting Gitmo-transfers.txt, make sure no
transfer is still "settling" on the Send screen.)


VERSION HISTORY
---------------
  0.9.12 Transfer requests and other pop-up cards now always show on top: in
         SMS, Settings and the lists they were drawn behind them, and their
         buttons only worked on a thin strip. The cracked glass covers those
         screens too.
  0.9.11 New built-in notification sound (a chime made for Gitmo). Your own
         sound still works: Gitmo-notify.wav next to the game.
  0.9.10 Photos taken with the phone: on Steam they go into your Steam
         screenshots, on Epic and other stores into Pictures\Gitmo (the
         photo panel lists that folder there).
  0.9.9  Photos taken with the phone's camera are saved to your Steam
         screenshots too (full size, listed in Steam like F12 ones).
  0.9.8  Stations your friends share now open when tapped (the link
         arrived on the same line as the text and wasn't recognised).
  0.9.7  Canceling a transfer tells the other player: their phone plays its
         notification sound and says "Transfer canceled". Stations shared
         over SMS are links: tap one to play it in Radio.
  0.9.6  The balance stays put when you drive through a tunnel to another
         map or region (it used to search again, and could show your other
         save's money for a few minutes).
  0.9.5  Home screen apps in a new order: SMS, Gitmo, Radio, Settings.
  0.9.4  The phone works in the main menu too (all apps; money transfers
         still need a loaded save). The radio no longer stops when you
         drive through a tunnel to another map, or in the main menu.
  0.9.3  Battery fix, properly this time: it still paused ("no truck")
         while driving some trucks.
  0.9.2  Cracked screen: about half the cracks, and more see-through.
  0.9.1  Battery fix: it paused ("no truck loaded yet") while driving some
         trucks; it charges and drains again. The radio stops when you
         go back to the main menu. The emoji and photo panels follow the
         phone's rounded corners. The Vibrate buzz is half as loud again.
  0.9.0  Radio app: internet radio with your own stations (add from a
         link, edit, delete), Browse (radio-browser.info), Now playing,
         Share over SMS, and a mini player on the home screen.
         The Vibrate notification is half as loud.
  0.8.4  Transfers work in garages again. When money leaves your balance,
         Gitmo presses Esc for the game once: an open shop, garage or
         refuel menu backs out and shows the right balance when reopened, so
         money you sent can't be spent there too (with no menu open, the
         pause menu opens). EscapeOnSend / EscapeVia in Gitmo.ini.
  0.8.3  (test build)
  0.8.2  Also when they accept: if you're in a garage at that moment, the
         transfer is cancelled and nothing moves (send it again outside).
  0.8.1  Money can't be sent from a garage: the garage screens keep the
         balance they read when they opened, so money sent away there could
         still be spent (a duplication). Gitmo also keeps track of a balance
         that a purchase took below zero.
  0.8.0  Photos in SMS: take a photo of the game view (without the phone) or
         send a Steam screenshot.
  0.7.6  Scrolling in VR: drag a list up or down (or use the mouse wheel),
         so all of Settings can be reached in the headset.
  0.7.5  Fixes 0.7.4 freezing at launch with the VR mod (Gitmo no longer
         hooks over another mod's DirectInput hooks).
  0.7.4  Colour emoji (no longer cut off in the text field). Settings >
         Notifications: Sound / Vibrate / Off, shown at the top of the phone.
         Keyboard keys typed on the phone no longer reach the game's controls
         (G honked the horn).
  0.7.3  Fixes 0.7.2 freezing at launch together with the VR mod (both
         mods wrapped the game's gamepad read in a loop).
  0.7.2  While the phone is open the game no longer sees gamepad / VR
         controller presses (they opened menus and the map). The emoji
         panel no longer closes right after opening. Wallpapers stay inside
         the phone's rounded corners; Gitmo shows your Steam picture.
  0.7.1  SMS emoji panel (smiley next to the text field). Typing on the
         phone no longer sets off the game's keyboard hotkeys.
  0.7.0  SMS app: text messages with Gitmo players, Steam pictures, online
         and offline contacts, kept history (erase it in Settings), delivered / read.
  0.6.2  The developer test buttons are hidden (they could change your
         money); Developer=1 in Gitmo.ini shows them.
  0.6.1  Transfers work for the player who joins a co-op game (not only the
         host): their balance is now certain as soon as they're in.
  0.6.0  Send lists only the players in your game session; transfers from
         anyone outside your game are turned down automatically
         (SessionOnly in Gitmo.ini). Gitmo reads the session from the
         game's own online service. Friends who appear offline in Steam
         can still connect.
  0.5.2  Activity keeps earlier sessions and days (per save); speaker icon
         in the status bar for the notification sound; the first save
         loaded after starting the game is recognised straight away; the
         balance card shows "$ -" (Checking) instead of a guess until Gitmo
         knows which save is loaded. Settings tidied up.
  0.5.1  Follows the save slot you load (all four slots); the phone only
         opens with a save loaded; declined / unanswered transfers show in
         Activity; notification sound with an on/off switch in Settings.
  0.5.0  Transfers between players over Steam: Send, with the receiver's
         Accept/Decline, refunds when a transfer can't finish, and labels
         in Activity ("To Bob", "From Bob").
  0.4.x  The phone as a panel in the VR headset (with the SnowRunner VR
         mod): drag it by the border, distance and pointer speed in
         Settings > VR. Fixed a freeze when used together with the VR mod.
  0.3    A real phone: home screen with apps, Settings app, battery, phone
         size, wallpapers, cracked screen.
  0.2    Finds the game's live money reliably; clicks on the phone no longer
         reach the game.
  0.1    Phone UI, live balance, activity.


CREDITS
-------
Phone UI drawn with Dear ImGui by Omar Cornut (MIT license), see
THIRD-PARTY-NOTICES.txt.
