#!/bin/sh

# Opt-in client configuration for the VA-API path.
#
# Only two clients are configured, and only in the user's own home:
#
#   mpv      ~/.config/mpv/mpv.conf with hwdec=vaapi. mpv's default is
#            hwdec=no, so a VA client that needs no environment still needs
#            this one line. The qualified run is TEST-RESULTS.md
#            "Client coverage": `--hwdec=vaapi --vo=gpu --gpu-context=wayland`.
#   Firefox  media.hardware-video-decoding.force-enabled in user.js, so
#            Firefox probes VA-API instead of deciding from its own
#            blocklist. This has NOT been verified as a Firefox decode run on
#            the qualified target; see README.md.
#
# What is deliberately not touched:
#   * Chrome. It needs no configuration: libva resolves this driver through
#     the msm_drv_video.so alias, and Chrome's packaged defaults are what this
#     project qualifies. No launcher, no feature override, no flags file.
#   * /etc. Nothing here needs root, and a system-wide policy file would
#     change the browser for every user on the machine.
#   * an existing mpv.conf. It is copied to mpv.conf.before-iris-vaapi once
#     and then left alone.
#
# Usage: iris-configure-clients.sh [--user NAME] [--dry-run]

set -u

target_user=''
dry_run=0

usage() {
    cat <<'EOF'
Usage: iris-configure-clients.sh [--user NAME] [--dry-run]

  --user NAME   configure this user's home (default: the current user)
  --dry-run     print what would be written, change nothing
  -h, --help    show this help
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --user) shift; [ "$#" -gt 0 ] || { echo 'iris-configure-clients: --user needs a name' >&2; exit 2; }; target_user=$1; shift ;;
        --dry-run) dry_run=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "iris-configure-clients: unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$target_user" ]; then
    target_user=$(id -un)
    home=$HOME
else
    home=$(getent passwd "$target_user" 2>/dev/null | cut -d: -f6)
    [ -n "$home" ] && [ -d "$home" ] \
        || { echo "iris-configure-clients: no home directory for $target_user" >&2; exit 1; }
fi

say() { printf '%s\n' "$*"; }
act() { [ "$dry_run" -eq 1 ] && say "would: $*" || say "$*"; }

# --- mpv ------------------------------------------------------------------
mpv_dir=$home/.config/mpv
mpv_conf=$mpv_dir/mpv.conf

mpv_ours() {
    grep -qx 'hwdec=vaapi' "$1" 2>/dev/null
}

if [ -f "$mpv_conf" ] && mpv_ours "$mpv_conf"; then
    say "mpv: already configured: $mpv_conf"
elif [ -f "$mpv_conf" ]; then
    backup=$mpv_conf.before-iris-vaapi
    if [ -e "$backup" ]; then
        say "mpv: $mpv_conf exists and $backup exists; left untouched" >&2
        say "     add 'hwdec=vaapi' by hand, or move one of them away" >&2
    else
        act "mpv: backing up $mpv_conf to $backup"
        [ "$dry_run" -eq 1 ] || cp -p "$mpv_conf" "$backup"
        # Replace the existing hwdec line rather than appending a second one:
        # mpv would take the last, but a file with two contradicting lines is
        # a trap for whoever reads it next.
        if grep -q '^[[:space:]]*hwdec[[:space:]]*=' "$mpv_conf"; then
            act "mpv: replacing the hwdec line in $mpv_conf"
            [ "$dry_run" -eq 1 ] || awk '
                !done && /^[[:space:]]*hwdec[[:space:]]*=/ { print "hwdec=vaapi"; done = 1; next }
                { print }
            ' "$backup" >"$mpv_conf"
        else
            act "mpv: appending hwdec=vaapi to $mpv_conf"
            [ "$dry_run" -eq 1 ] || printf '\n# Qualcomm Iris through VA-API (added by iris-configure-clients.sh)\nhwdec=vaapi\n' \
                >>"$mpv_conf"
        fi
    fi
else
    act "mpv: writing $mpv_conf"
    if [ "$dry_run" -eq 0 ]; then
        mkdir -p "$mpv_dir"
        cat >"$mpv_conf" <<'EOF'
# Hardware decode through the Qualcomm Iris codec, using VA-API.
# `hwdec=vaapi` is the path this project qualifies; the native V4L2 M2M path
# (`hwdec=v4l2m2m-copy`) does not use the VA driver at all.
hwdec=vaapi
vo=gpu
gpu-context=wayland
EOF
    fi
fi

# --- Firefox --------------------------------------------------------------
# user.js is read at every start and Firefox never rewrites it, unlike
# prefs.js. The profile directory comes from profiles.ini: an [Install...]
# section wins, otherwise the [Profile*] with Default=1. Writing to every
# *.default* directory would also hit stale profiles that are never opened.
firefox_root=$home/.mozilla/firefox
profiles_ini=$firefox_root/profiles.ini

firefox_profile() {
    [ -f "$profiles_ini" ] || return 1
    awk '
        /^\[/ { section = $0; sub(/^\[/, "", section); sub(/\]$/, "", section); next }
        /^Default=/ && section ~ /^Install/ { print "install " substr($0, 9); found = 1; exit }
        /^Default=1/ && section ~ /^Profile/ { current = section; profile_default = 1 }
        /^Path=/ && section ~ /^Profile/ && profile_default { path = substr($0, 6); relative = 1 }
        /^IsRelative=/ && section ~ /^Profile/ { relative = substr($0, 12) }
        END {
            if (found) exit
            if (path != "") print (relative ? "relative " : "absolute ") path
        }
    ' "$profiles_ini"
}

if [ ! -d "$firefox_root" ]; then
    say "firefox: no $firefox_root; start Firefox once, then re-run"
else
    selection=$(firefox_profile) || selection=''
    case "$selection" in
        'install '*) profile_path=${selection#install }; profile_base=$firefox_root ;;
        'relative '*) profile_path=${selection#relative }; profile_base=$firefox_root ;;
        'absolute '*) profile_path=${selection#absolute }; profile_base='' ;;
        *) profile_path=''; profile_base='' ;;
    esac
    if [ -z "$profile_path" ]; then
        say "firefox: $profiles_ini names no default profile; start Firefox once, then re-run"
    else
        profile_dir=$profile_base/$profile_path
        user_js=$profile_dir/user.js
        if [ ! -d "$profile_dir" ]; then
            say "firefox: profile directory is missing: $profile_dir" >&2
        elif grep -q 'media.hardware-video-decoding.force-enabled' "$user_js" 2>/dev/null; then
            say "firefox: already configured: $user_js"
        else
            act "firefox: writing $user_js"
            if [ "$dry_run" -eq 0 ]; then
                {
                    printf '%s\n' \
                        '// Qualcomm Iris through VA-API (added by iris-configure-clients.sh).' \
                        '// media.ffmpeg.vaapi.enabled was removed in Firefox 137; this is the' \
                        '// pref that makes Firefox probe VA-API regardless of its blocklist.' \
                        'user_pref("media.hardware-video-decoding.force-enabled", true);'
                } >>"$user_js"
            fi
        fi
    fi
fi

say ''
say 'Fully close and reopen the client for this to take effect.'
