#!/usr/bin/env python3
"""Attach to an already-playing Chrome page and collect Media evidence.

Unlike remote/cdp.py run, this does not navigate: the launcher has already
opened the page. It enables the Media domain, reads getVideoPlaybackQuality()
twice over the window, and reports the selected decoder with frame/drop deltas.
Hardware is decided by the caller from decoder_name and the window.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) + "/remote")
from cdp import Connection, first_page, media_properties, pick_video_player  # noqa: E402

STATS = """(() => {
  const vids=[];
  const walk=(r)=>{ (r.querySelectorAll?r.querySelectorAll('video'):[]).forEach(v=>vids.push(v));
    (r.querySelectorAll?r.querySelectorAll('*'):[]).forEach(e=>{if(e.shadowRoot)walk(e.shadowRoot);}); };
  walk(document);
  const v=vids.find(x=>x.getVideoPlaybackQuality?x.getVideoPlaybackQuality().totalVideoFrames>0:false)||vids[0];
  if(!v) return {n:0,href:location.href,title:document.title};
  const q=v.getVideoPlaybackQuality?v.getVideoPlaybackQuality():null;
  return {n:vids.length,href:location.href,title:document.title,
    decoded:q?q.totalVideoFrames:null,dropped:q?q.droppedVideoFrames:null,
    w:v.videoWidth,h:v.videoHeight,paused:v.paused,ct:v.currentTime};
})()"""


def main():
    port, seconds, out = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
    target = first_page(port)
    c = Connection(target["webSocketDebuggerUrl"])
    try:
        c.call("Runtime.enable", timeout=10)
        c.call("Media.enable", timeout=10)
        # Let playerPropertiesChanged events arrive before the window opens.
        c.drain(3)
        first = c.evaluate(STATS, timeout=12)
        c.drain(seconds)
        second = c.evaluate(STATS, timeout=12)
        players = media_properties(c.events)
        _, props = pick_video_player(players)
        platform = props.get("kIsPlatformVideoDecoder")
        if isinstance(platform, str):
            platform = platform.lower() == "true"
        report = {
            "page_url": target.get("url"),
            "decoder_name": props.get("kVideoDecoderName"),
            "is_platform_video_decoder": platform,
            "player_count": len(players),
            "stats_first": first,
            "stats_second": second,
            "media_properties": props,
        }
        if isinstance(first.get("decoded"), int) and isinstance(second.get("decoded"), int):
            report["window_decoded_frames"] = second["decoded"] - first["decoded"]
            report["window_dropped_frames"] = (second.get("dropped") or 0) - (first.get("dropped") or 0)
    finally:
        c.close()
    with open(out, "w") as f:
        json.dump(report, f, indent=2, sort_keys=True)
        f.write("\n")
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
