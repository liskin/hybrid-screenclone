This program has been obsoleted by xserver-xorg-video-intel 2.99.

http://cgit.freedesktop.org/xorg/driver/xf86-video-intel/tree/tools/

# hybrid-screenclone

This is a reimplementation of [hybrid-windump][hybrid-windump] with the
opposite use-case: doing all rendering using the integrated intel card and
using the additional card just to get more outputs (e.g. a triple-head with
ThinkPad T420). As such, it uses the DAMAGE extension to avoid unnecessary
redraws and the RECORD extension to capture mouse movements, which are
translated to mouse movements on the destination X server.

For this to work correctly, an additional virtual Xinerama screen must be
available. To get one, see my [virtual CRTC for intel][patch] patch.

[hybrid-windump]: https://github.com/harp1n/hybrid-windump
[patch]: https://github.com/liskin/patches/blob/master/hacks/xserver-xorg-video-intel-2.18.0_virtual_crtc.patch
