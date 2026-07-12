"""路网流量图 / OD 期望线图 — gRPC 与 AI 报告共用渲染。"""
from __future__ import annotations

import html
import json
import os
from typing import Any

_DEFAULT_TDT_TK = os.environ.get("TNA_TIANDITU_TK", "2e48aa1ca86dd64106f2ae49459c7661")

MAP_EXTRA_CSS = """
.map-box{height:480px;width:100%;border:1px solid #d8e0e8;border-radius:8px;z-index:1;background:#f7f8f4;box-shadow:0 2px 10px rgba(15,23,42,.10);overflow:hidden}
.map-box.compare{height:420px}
.map-box .tdt-base-tile,.map-box .fallback-base-tile{filter:brightness(1.25) contrast(.93) saturate(.93) grayscale(30%) opacity(.78)}
.map-box .tdt-anno-tile{filter:brightness(1.08) contrast(.96) saturate(.94) opacity(.72)}
.map-box .leaflet-tile-pane::after{content:"";position:absolute;inset:0;background:rgba(255,255,255,.30);pointer-events:none;z-index:450}
.map-stack{display:flex;flex-direction:column;gap:16px}
.map-subtitle{font-size:13px;font-weight:600;color:#1b4a82;margin:0 0 8px}
.map-legend{font-size:12px;color:#475569;margin:8px 0 12px;background:rgba(255,255,255,.88);border:1px solid #e2e8f0;border-radius:999px;padding:8px 12px;display:inline-block;box-shadow:0 1px 6px rgba(15,23,42,.08)}
.map-legend span{display:inline-block;margin-right:14px}
.map-legend i{display:inline-block;width:28px;height:5px;border-radius:999px;vertical-align:middle;margin-right:5px;box-shadow:0 0 0 1px rgba(255,255,255,.85)}
.map-legend .od-w-thin{display:inline-block;width:28px;height:2px;background:#2c7bb6;vertical-align:middle;margin:0 4px}
.map-legend .od-w-mid{display:inline-block;width:28px;height:5px;background:#fdae61;vertical-align:middle;margin:0 4px}
.map-legend .od-w-thick{display:inline-block;width:28px;height:9px;background:#d7191c;vertical-align:middle;margin:0 4px}
.map-legend .zone-fill{display:inline-block;width:16px;height:16px;background:rgba(255,255,255,0.50);border:0.6px solid #111;vertical-align:middle;margin:0 4px}
.road-name-label,.od-pair-label,.zone-id-label{background:none!important;border:none!important}
.road-name-label span,.od-pair-label span,.zone-id-label span{
  display:inline-block;padding:1px 4px;border-radius:3px;font-size:10px;font-weight:600;line-height:1.2;
  white-space:nowrap;pointer-events:none;text-shadow:-1px -1px 2px #fff,1px -1px 2px #fff,-1px 1px 2px #fff,1px 1px 2px #fff}
.road-name-label span{color:#1a5276}
.od-pair-label span{color:#922b21;background:rgba(255,255,255,0.75);font-size:9px}
.zone-id-label span{color:#1f618d;background:rgba(255,255,255,0.8)}
.leaflet-od-legend{padding:10px 12px;background:rgba(255,255,255,0.92);border:1px solid #d9dde4;border-radius:8px;font-size:13px;line-height:1.55;color:#1f2937;box-shadow:0 2px 10px rgba(15,23,42,0.18)} .leaflet-od-legend b{display:block;font-size:14px;margin-bottom:4px;color:#111827}.leaflet-od-legend .leg-row{display:flex;align-items:center;gap:6px;white-space:nowrap}.leaflet-od-legend .leg-line{display:inline-block;width:34px;height:3px;border-radius:999px}.leaflet-od-legend .leg-line.c1{height:1.5px}.leaflet-od-legend .leg-line.c2{height:3px}.leaflet-od-legend .leg-line.c3{height:4.5px}.leaflet-od-legend .leg-line.c4{height:6px}.leaflet-od-legend .leg-line.c5{height:8px}.leaflet-od-legend .leg-zone{display:inline-block;width:14px;height:14px;background:rgba(255,255,255,0.50);border:0.6px solid #111}.leaflet-od-legend .leg-dot{display:inline-block;width:8px;height:8px;background:#1a5276}.leaflet-od-legend .leg-od{display:inline-block;width:8px;height:8px;background:#922b21}
details.map-fold{margin-bottom:14px;border:1px solid #dce4ec;border-radius:8px;padding:10px 14px;background:#fafbfc}
details.map-fold>summary{cursor:pointer;font-size:14px;font-weight:600;color:#1a3a5c;list-style-position:outside}
details.map-fold[open]>summary{margin-bottom:10px}
.map-empty{font-size:12px;color:#856404;line-height:1.8;padding:8px 4px}
.map-warn{font-size:13px;color:#7b241c;background:#fdecea;border:1px solid #f5b7b1;border-radius:6px;padding:10px 12px;margin:0 0 10px;line-height:1.65}
.map-fold-body{padding-top:4px}
.map-compare-grid{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-top:8px}
.map-compare-col h4{font-size:13px;font-weight:600;color:#1b4a82;margin:0 0 8px;text-align:center}
.map-dir-toolbar{margin:8px 0 12px;font-size:13px;color:#444;display:flex;flex-wrap:wrap;align-items:center;gap:8px}
.map-dir-toolbar select{padding:4px 10px;border:1px solid #c5d3e0;border-radius:4px;font-size:13px}
.map-dir-hint{font-size:12px;color:#666}
@media(max-width:900px){.map-compare-grid{grid-template-columns:1fr}}
"""

MAP_INIT_JS = r"""
(function(){
  var _roadMaps = [];
  function showMapError(id, msg) {
    var el = document.getElementById(id);
    if (el) el.innerHTML = '<p style="padding:14px;color:#c0392b;font-size:13px">' + msg + '</p>';
  }
  function addTdt(map, tk) {
    function layer(kind) {
      return L.tileLayer(
        'https://t{s}.tianditu.gov.cn/DataServer?T=' + kind + '_w&x={x}&y={y}&l={z}&tk=' + encodeURIComponent(tk),
        {subdomains:'01234567', maxZoom:18, opacity: kind==='vec'?0.72:0.95, className: kind==='vec'?'tdt-base-tile':'tdt-anno-tile'}
      );
    }
    var base = layer('vec'), anno = layer('cva');
    base.addTo(map); anno.addTo(map);
    base.on('tileerror', function() {
      if (map._cnFallback) return;
      map._cnFallback = true;
      try { map.removeLayer(base); map.removeLayer(anno); } catch(e) {}
      var fallbacks = [
        'https://map.geoq.cn/ArcGIS/rest/services/ChinaOnlineStreetWarm/MapServer/tile/{z}/{y}/{x}',
        'https://map.geoq.cn/ArcGIS/rest/services/ChinaOnlineStreetGray/MapServer/tile/{z}/{y}/{x}',
        'https://webrd0{s}.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=8&x={x}&y={y}&z={z}'
      ];
      var idx = 0;
      function tryNext() {
        if (idx >= fallbacks.length) return;
        var url = fallbacks[idx++];
        var lyr = L.tileLayer(url, {subdomains:'1234', maxZoom:18, opacity:0.88, className:'fallback-base-tile'});
        lyr.on('tileerror', tryNext);
        lyr.addTo(map);
      }
      tryNext();
    });
  }
  function fitMap(map, bounds, latLngs, center) {
    if (bounds && bounds.length===2) {
      map.fitBounds(bounds, {padding:[36,36], maxZoom:16});
    } else if (latLngs.length) {
      map.fitBounds(latLngs, {padding:[36,36], maxZoom:16});
    } else {
      map.setView(center || [30.67, 104.06], 12);
    }
  }
  function vcColor(v) {
    if (v == null) return '#5d6d7e';
    if (v >= 0.8) return '#d7191c';
    if (v >= 0.6) return '#fdae61';
    if (v >= 0.4) return '#ffffbf';
    if (v >= 0.2) return '#a6d96a';
    return '#1a9641';
  }
  function vcWeight(v) {
    if (v == null) return 2.4;
    if (v >= 0.8) return 5.8;
    if (v >= 0.6) return 4.8;
    if (v >= 0.4) return 3.8;
    if (v >= 0.2) return 3.0;
    return 2.4;
  }
  function odClass(demand, minD, maxD) {
    var lo = minD || 1, hi = maxD || lo;
    if (hi <= lo) return 2;
    return Math.max(0, Math.min(4, Math.floor(((demand - lo) / (hi - lo)) * 5)));
  }
  function odColor(cls) {
    return ['#2c7bb6','#abd9e9','#ffffbf','#fdae61','#d7191c'][cls] || '#d7191c';
  }
  function odWeight(cls) {
    // Leaflet 像素线宽按 GIS 0.2/0.4/0.6/0.8/1.0 做等比例放大，保证网页上可见。
    return [1.2,2.4,3.6,4.8,6.0][cls] || 3.6;
  }
  function lineMid(ll) {
    if (!ll || !ll.length) return null;
    var a = ll[0], b = ll[ll.length - 1];
    return [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];
  }
  function divLabel(cls, text) {
    return L.marker([0, 0], {
      icon: L.divIcon({
        className: cls,
        html: '<span>' + text + '</span>',
        iconSize: [0, 0],
        iconAnchor: [0, 0]
      }),
      interactive: false
    });
  }
  function drawZonePolygons(map, polys, style) {
    var layer = L.layerGroup();
    (polys || []).forEach(function(z) {
      (z.rings || []).forEach(function(ring) {
        if (!ring || ring.length < 3) return;
        var tip = '小区 ' + z.area_id + (z.name ? (' · ' + z.name) : '');
        L.polygon(ring, style).bindTooltip(tip, {sticky:true}).addTo(layer);
      });
    });
    layer.addTo(map);
    return layer;
  }
  function offsetLine(latlng, side, amt) {
    if (!latlng || latlng.length < 2) return latlng;
    var out = [];
    for (var i = 0; i < latlng.length; i++) {
      var a = latlng[i];
      var b = latlng[Math.min(i + 1, latlng.length - 1)];
      var dlat = b[0] - a[0], dlng = b[1] - a[1];
      var len = Math.sqrt(dlat * dlat + dlng * dlng) || 1;
      var ox = (-dlng / len) * amt * side;
      var oy = (dlat / len) * amt * side;
      out.push([a[0] + oy, a[1] + ox]);
    }
    return out;
  }
  function filterLinks(links, mode) {
    links = links || [];
    if (mode === 'forward') return links.filter(function(l){ return l.dir !== 'reverse'; });
    if (mode === 'reverse') return links.filter(function(l){ return l.dir === 'reverse'; });
    return links;
  }
  function drawRoadLayer(map, roadData, dirMode) {
    var hasFlow = roadData.hasAssignmentFlow === true;
    var latLngs = [];
    var links = filterLinks(roadData.links || [], dirMode);
    (roadData.centroid_connectors || []).forEach(function(c) {
      if (!c.latlng || c.latlng.length < 2) return;
      L.polyline(c.latlng, {
        color:'#8064a2', weight:0.8, opacity:0.16, dashArray:'2 7', lineCap:'round'
      }).bindTooltip('质心连杆 type=10 \u00b7 小区' + c.zone_id, {sticky:true}).addTo(map);
      c.latlng.forEach(function(p) { latLngs.push(p); });
    });
    links.forEach(function(l) {
      if (!l.latlng || l.latlng.length < 2) return;
      var ll = l.latlng;
      var dash = null;
      if (dirMode === 'split') {
        ll = offsetLine(l.latlng, l.dir === 'reverse' ? -1 : 1, 0.000035);
      } else if (dirMode === 'reverse' && l.dir === 'reverse') {
        dash = '6 4';
      }
      var tip = (l.name || ('路段' + l.link_id)) + ' #' + l.link_id;
      if (l.init_node != null && l.term_node != null) tip += ' | ' + l.init_node + '\u2192' + l.term_node;
      if (hasFlow && l.v_c != null) tip += ' | V/C=' + l.v_c;
      else if (!hasFlow) tip += ' | \u65e0\u5206\u914d\u6d41\u91cf';
      var mainWeight = hasFlow ? vcWeight(l.v_c) : 2.4;
      L.polyline(ll, {
        color: hasFlow ? vcColor(l.v_c) : '#5b7c99',
        weight: mainWeight,
        opacity: hasFlow ? 1.0 : 0.82,
        dashArray: dash,
        lineCap: 'butt',
        lineJoin: 'miter'
      }).bindTooltip(tip, {sticky:true, direction:'top', opacity:0.95}).addTo(map);
      ll.forEach(function(p) { latLngs.push(p); });
    });
    fitMap(map, roadData.bounds, latLngs, roadData.center);
  }
  function mountRoadMap(elId, roadData, dirMode) {
    var el = document.getElementById(elId);
    if (!el || !roadData || !roadData.links || !roadData.links.length) return null;
    el.innerHTML = '';
    var map = L.map(elId, {zoomControl:true, attributionControl:true});
    addTdt(map, roadData.tdtTk || '');
    drawRoadLayer(map, roadData, dirMode || 'split');
    setTimeout(function() { map.invalidateSize(); }, 280);
    return map;
  }
  function drawOdMap(odData) {
    if (!document.getElementById('tna-map-od') || !odData.od_lines || !odData.od_lines.length) return;
    var omap = L.map('tna-map-od', {zoomControl:true, attributionControl:true});
    addTdt(omap, odData.tdtTk || '');
    var minD = odData.demand_min || 1, maxD = odData.demand_max || 1, latLngs = [];
    var zoneLayer = L.layerGroup();
    var odLayer = L.layerGroup();
    var labelLayer = L.layerGroup();
    drawZonePolygons(omap, odData.zone_polygons, {
      color:'#111111', weight:0.6, opacity:0.85, fillColor:'#ffffff', fillOpacity:0.50
    });
    (odData.zone_points || []).forEach(function(z) {
      if (!z.latlng) return;
      L.circleMarker(z.latlng, {
        radius:6, color:'#1a5276', fillColor:'#2980b9', fillOpacity:0.9, weight:2
      }).bindTooltip('小区 ' + z.area_id, {sticky:true}).addTo(zoneLayer);
      divLabel('zone-id-label', String(z.area_id)).setLatLng(z.latlng).addTo(labelLayer);
      latLngs.push(z.latlng);
    });
    (odData.od_lines || []).forEach(function(o) {
      var ll = (o.coordinates || []).map(function(c) { return [c[1], c[0]]; });
      if (ll.length < 2) return;
      var cls = odClass(o.demand, minD, maxD);
      var w = odWeight(cls);
      L.polyline(ll, {
        color:odColor(cls), weight:w, opacity:0.94, lineCap:'butt', lineJoin:'miter'
      }).bindTooltip('O' + o.f_id + '\u2192D' + o.t_id + ' \u9700\u6c42=' + o.demand, {sticky:true}).addTo(odLayer);
      var mid = lineMid(ll);
      if (mid && cls >= 3) divLabel('od-pair-label', o.f_id + '\u2192' + o.t_id).setLatLng(mid).addTo(labelLayer);
      ll.forEach(function(p) { latLngs.push(p); });
    });
    zoneLayer.addTo(omap); odLayer.addTo(omap); labelLayer.addTo(omap);
    var odLegend = L.control({position:'bottomright'});
    odLegend.onAdd = function() {
      var div = L.DomUtil.create('div', 'leaflet-od-legend');
      div.innerHTML = '<b>OD \u56fe\u4f8b</b>'
        + '<div class="leg-row"><span class="leg-line c1" style="background:#2c7bb6"></span>低需求</div>'
        + '<div class="leg-row"><span class="leg-line c2" style="background:#abd9e9"></span>较低需求</div>'
        + '<div class="leg-row"><span class="leg-line c3" style="background:#ffffbf"></span>中需求</div>'
        + '<div class="leg-row"><span class="leg-line c4" style="background:#fdae61"></span>较高需求</div>'
        + '<div class="leg-row"><span class="leg-line c5" style="background:#d7191c"></span>高需求</div>'
        + '<div class="leg-row"><span class="leg-zone"></span>小区面</div>'
        + '<div class="leg-row"><span class="leg-dot"></span>小区 ID</div>'
        + '<div>需求 ' + minD + ' - ' + maxD + '</div>';
      return div;
    };
    odLegend.addTo(omap);
    fitMap(omap, odData.bounds, latLngs, odData.center);
    setTimeout(function() { omap.invalidateSize(); }, 320);
  }
  function refreshRoadMaps(bundle, dirMode) {
    _roadMaps.forEach(function(m) { try { m.remove(); } catch(e) {} });
    _roadMaps = [];
    if (bundle.mode === 'compare') {
      (bundle.layers || []).forEach(function(layer) {
        var m = mountRoadMap('tna-map-road-' + layer.key, layer.road, dirMode);
        if (m) _roadMaps.push(m);
      });
    } else if (bundle.road && bundle.road.links && bundle.road.links.length) {
      var m = mountRoadMap('tna-map-road', bundle.road, dirMode);
      if (m) _roadMaps.push(m);
    }
  }
  function initTnaMaps() {
    try {
      if (typeof L === 'undefined') {
        showMapError('tna-map-road', 'Leaflet \u5e93\u672a\u52a0\u8f7d');
        return;
      }
      var raw = document.getElementById('tna-map-bundle');
      if (!raw) return;
      var bundle = JSON.parse(raw.textContent);
      var sel = document.getElementById('tna-map-dir-filter');
      var dirMode = (sel && sel.value) ? sel.value : (bundle.directionDefault || 'split');
      refreshRoadMaps(bundle, dirMode);
      if (sel) {
        sel.value = dirMode;
        sel.addEventListener('change', function() {
          refreshRoadMaps(bundle, sel.value);
        });
      }
      if (bundle.od && bundle.od.od_lines && bundle.od.od_lines.length) {
        drawOdMap(bundle.od);
      }
    } catch (e) {
      console.error('TNA map init failed', e);
      showMapError('tna-map-road', '\u5730\u56fe\u6e32\u67d3\u5931\u8d25: ' + e.message);
    }
  }
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initTnaMaps);
  } else {
    setTimeout(initTnaMaps, 0);
  }
})();
"""


def leaflet_head_html() -> str:
    return (
        '<link rel="stylesheet" href="https://cdn.bootcdn.net/ajax/libs/leaflet/1.9.4/leaflet.min.css"/>'
        '<script src="https://cdn.bootcdn.net/ajax/libs/leaflet/1.9.4/leaflet.min.js"></script>'
    )


def _json_for_html_script(obj: Any) -> str:
    return json.dumps(obj, ensure_ascii=False).replace("</", "<\\/")


def _road_empty_reason(mmap: dict) -> str:
    if not mmap.get("ok"):
        return mmap.get("reason") or "缺少 road_way 表或 geometry 字段为空；请先完成交通分配。"
    if not mmap.get("links"):
        return "road_way 无有效几何或流量字段，无法绘制路网分布图。"
    return "暂无路网流量数据。"


def _od_empty_reason(od: dict) -> str:
    if od.get("pair_count"):
        return (
            "有 OD 出行量（{} 对），但期望线几何缺失或为区内零长度，无法绘制。"
        ).format(od.get("pair_count"))
    if not od.get("ok"):
        return od.get("reason") or "缺少 other_od 表或 OD 数据为空。"
    return "暂无 OD 期望线数据（需 other_od 与质心连杆 type=10 或交通小区几何）。"


def _road_map_note(mmap: dict, pipeline: dict | None) -> str:
    assign = mmap.get("assignment") or {}
    if assign.get("has_flow") and assign.get("has_vc_coloring"):
        return ""
    pl = pipeline or {}
    parts = []
    if not assign.get("has_flow"):
        parts.append(
            assign.get("note")
            or "交通分配未成功或流量为空，地图仅展示路网拓扑，不按 V/C 着色。"
        )
        if pl.get("assign_error"):
            parts.append(str(pl["assign_error"]))
    elif not assign.get("has_vc_coloring"):
        parts.append("已有分配记录但未解析到有效 V/C，地图以拓扑线展示。")
    return " ".join(p for p in parts if p)


def _pack_road_layer(
    nb: dict[str, Any],
    *,
    pipeline: dict[str, Any] | None = None,
    map_mode: str = "motor",
) -> dict[str, Any]:
    if map_mode == "slow":
        mmap = nb.get("slow_map") or {}
        centroid = nb.get("slow_centroid") or {}
    else:
        mmap = nb.get("map") or {}
        centroid = nb.get("centroid") or {}
    assign = mmap.get("assignment") or {}
    has_vc = bool(assign.get("has_vc_coloring"))
    return {
        "center": mmap.get("center") or [30.67, 104.06],
        "bounds": mmap.get("bounds"),
        "links": mmap.get("links") or [],
        "centroid_connectors": centroid.get("connectors") or [],
        "hasAssignmentFlow": has_vc,
        "tdtTk": _DEFAULT_TDT_TK,
        "note": _road_map_note(mmap, pipeline),
    }


def _pack_od_layer(nb: dict[str, Any], *, map_mode: str = "motor") -> dict[str, Any]:
    if map_mode == "slow":
        mmap = nb.get("slow_map") or nb.get("map") or {}
        centroid = nb.get("slow_centroid") or nb.get("centroid") or {}
    else:
        mmap = nb.get("map") or {}
        centroid = nb.get("centroid") or {}
    od = nb.get("od") or {}
    return {
        "center": od.get("map_center") or mmap.get("center") or [30.67, 104.06],
        "bounds": od.get("map_bounds") or mmap.get("bounds"),
        "od_lines": od.get("lines") or [],
        "zone_points": centroid.get("zones") or [],
        "zone_polygons": centroid.get("zone_polygons") or [],
        "demand_min": od.get("demand_min"),
        "demand_max": od.get("demand_max"),
        "tdtTk": _DEFAULT_TDT_TK,
    }


def _direction_toolbar_html() -> str:
    return """
      <div class="map-dir-toolbar">
        <label for="tna-map-dir-filter"><strong>行驶方向筛选</strong></label>
        <select id="tna-map-dir-filter">
          <option value="split" selected>双向分离（推荐，缓解重合）</option>
          <option value="forward">仅正向 init→term</option>
          <option value="reverse">仅反向 term→init</option>
          <option value="all">全部（双向重合）</option>
        </select>
        <span class="map-dir-hint">双向道路几何重合时，请用「双向分离」或按方向筛选；悬停可见节点方向与 V/C。</span>
      </div>"""


def _vc_legend_html(*, topology: bool = False) -> str:
    if topology:
        return """
      <p class="map-legend">
        <span><i style="background:#5b7c99"></i>无 V/C / 无分配流量路段</span>
        <span><i style="background:#8064a2;height:3px;opacity:.25"></i>质心连杆 type=10</span>
      </p>"""
    return """
      <p class="map-legend">
        <span><i style="background:#1a9641"></i>0-0.2</span>
        <span><i style="background:#a6d96a"></i>0.2-0.4</span>
        <span><i style="background:#ffffbf"></i>0.4-0.6</span>
        <span><i style="background:#fdae61"></i>0.6-0.8</span>
        <span><i style="background:#d7191c"></i>0.8以上</span>
        <span><i style="background:#5b7c99"></i>无 V/C / 无分配流量路段</span>
        <span><i style="background:#8064a2;height:3px;opacity:.25"></i>质心连杆 type=10</span>
        <span>统一口径：Greedy 分配 flow/capacity → V/C</span>
      </p>"""


def build_map_bundle_payload(
    nb: dict[str, Any] | None,
    *,
    pipeline: dict[str, Any] | None = None,
    compare_layers: list[dict[str, Any]] | None = None,
    map_mode: str = "motor",
) -> dict[str, Any]:
    """构建前端 tna-map-bundle JSON。"""
    if compare_layers:
        layers_out = []
        od_src = None
        for i, layer in enumerate(compare_layers):
            bundle = layer.get("bundle") or {}
            pl = layer.get("pipeline") or pipeline
            road = _pack_road_layer(bundle, pipeline=pl, map_mode=map_mode)
            key = layer.get("key") or f"layer{i}"
            layers_out.append({
                "key": key,
                "label": layer.get("label") or key,
                "case_id": layer.get("case_id"),
                "road": road,
            })
            if od_src is None and (bundle.get("od") or {}).get("lines"):
                od_src = bundle
        payload: dict[str, Any] = {
            "mode": "compare",
            "directionDefault": "split",
            "tdtTk": _DEFAULT_TDT_TK,
            "layers": layers_out,
        }
        if od_src:
            payload["od"] = _pack_od_layer(od_src, map_mode=map_mode)
        return payload

    nb = nb or {}
    mmap = (nb.get("slow_map") if map_mode == "slow" else nb.get("map")) or {}
    od = nb.get("od") or {}
    has_road = bool(mmap.get("ok") and mmap.get("links"))
    has_od = bool((od.get("lines") or []))
    if not has_road and not has_od:
        return {}
    payload = {
        "mode": "single",
        "directionDefault": "split",
        "tdtTk": _DEFAULT_TDT_TK,
        "road": _pack_road_layer(nb, pipeline=pipeline, map_mode=map_mode),
    }
    if has_od:
        payload["od"] = _pack_od_layer(nb, map_mode=map_mode)
    return payload


def render_map_sections(
    nb: dict[str, Any] | None,
    *,
    pipeline: dict[str, Any] | None = None,
    compare_layers: list[dict[str, Any]] | None = None,
    section_title: str = "空间分布：路网流量对比（统一 V/C 口径）",
    map_mode: str = "motor",
) -> dict[str, Any]:
    """
    返回路网/OD 折叠区块 HTML、初始化 JS、Leaflet head。
    compare_layers: [{key, label, case_id, bundle, pipeline}, ...] 用于方案对比双图。
    """
    map_mode = "slow" if map_mode == "slow" else "motor"
    mode_label = "\u6162\u884c" if map_mode == "slow" else "\u673a\u52a8\u8f66"
    bundle_payload = build_map_bundle_payload(
        nb, pipeline=pipeline, compare_layers=compare_layers, map_mode=map_mode
    )
    is_compare = bool(compare_layers) and bundle_payload.get("mode") == "compare"
    layers = bundle_payload.get("layers") or []
    road = bundle_payload.get("road") or {}
    od = bundle_payload.get("od") or {}
    has_road = is_compare and any(
        (ly.get("road") or {}).get("links") for ly in layers
    ) or bool(road.get("links"))
    has_od = bool(od.get("od_lines"))
    has_vc_any = is_compare and any(
        (ly.get("road") or {}).get("hasAssignmentFlow") for ly in layers
    ) or bool(road.get("hasAssignmentFlow"))

    bundle_json = ""
    if bundle_payload:
        bundle_json = _json_for_html_script(bundle_payload)

    od_lines = od.get("od_lines") or []
    d_min, d_max = od.get("demand_min"), od.get("demand_max")
    od_demand_legend = ""
    if d_min is not None and d_max is not None:
        od_demand_legend = (
            f"<span>需求 {html.escape(str(d_min))} – {html.escape(str(d_max))}</span>"
        )

    parts: list[str] = [
        '<div id="s_maps" class="section">',
        f'<div class="sec-title"><span class="dot dm"></span>{html.escape(section_title)}</div>',
        '<p class="desc" style="margin-bottom:14px">'
        "多方案对比时并排展示相同指标（路段 V/C = flow÷capacity）；"
        "无分配结果时仅绘拓扑线并标注原因。"
        "支持行驶方向筛选以缓解双向路段几何重合。</p>",
    ]
    if bundle_json:
        parts.append(f'<script type="application/json" id="tna-map-bundle">{bundle_json}</script>')

    road_open = " open" if has_road else ""
    if is_compare:
        road_summary = f"{mode_label}\u8def\u7f51 V/C \u5bf9\u6bd4\uff08\u57fa\u7840\u65b9\u6848 vs \u672c\u65b9\u6848\uff09"
    elif has_road and has_vc_any:
        road_summary = f"{mode_label}\u8def\u7f51\u6d41\u91cf\u5206\u5e03\uff08V/C \u7740\u8272\uff09"
    elif has_road:
        road_summary = f"{mode_label}\u8def\u7f51\u7ed3\u6784\uff08\u65e0\u5206\u914d\u6d41\u91cf\uff0c\u4ec5\u62d3\u6251\uff09"
    else:
        mmap0 = ((nb or {}).get("slow_map") if map_mode == "slow" else (nb or {}).get("map")) or {}
        road_summary = f"{mode_label}\u8def\u7f51\uff08\u6682\u65e0\uff1a{html.escape(_road_empty_reason(mmap0))}\uff09"

    parts.append(f'<details class="map-fold" id="fold-map-road"{road_open}>')
    parts.append(f"<summary>{road_summary}</summary>")
    parts.append('<div class="map-fold-body">')

    if is_compare:
        for ly in layers:
            note = (ly.get("road") or {}).get("note") or ""
            if note:
                parts.append(
                    f'<p class="map-warn">{html.escape(ly.get("label", ""))}：{html.escape(note)}</p>'
                )
        parts.append(_vc_legend_html(topology=not has_vc_any))
        parts.append(_direction_toolbar_html())
        parts.append('<div class="map-compare-grid">')
        for ly in layers:
            key = html.escape(str(ly.get("key") or "x"))
            label = html.escape(str(ly.get("label") or key))
            parts.append(
                f'<div class="map-compare-col">'
                f'<h4>{label}</h4>'
                f'<div id="tna-map-road-{key}" class="map-box compare"></div>'
                f"</div>"
            )
        parts.append("</div>")
    elif has_road:
        note = road.get("note") or ""
        if note:
            parts.append(f'<p class="map-warn">{html.escape(note)}</p>')
        parts.append(_vc_legend_html(topology=not has_vc_any))
        parts.append(_direction_toolbar_html())
        parts.append('<div id="tna-map-road" class="map-box"></div>')
    else:
        mmap0 = (nb or {}).get("map") or {}
        parts.append(f'<p class="map-empty">{html.escape(_road_empty_reason(mmap0))}</p>')
    parts.append("</div></details>")

    od_open = " open" if has_od else ""
    od_summary = (
        f"OD 期望线分布（Top {len(od_lines)} 对）"
        if has_od
        else f"OD 期望线分布（暂无：{html.escape(_od_empty_reason((nb or {}).get('od') or {}))}）"
    )
    parts.append(f'<details class="map-fold" id="fold-map-od"{od_open}>')
    parts.append(f"<summary>{od_summary}</summary>")
    parts.append('<div class="map-fold-body">')
    if has_od:
        parts.append(
            f"""
      <p class="map-legend">
        <span><i style="background:#2c7bb6;height:1.2px"></i>低需求</span>
        <span><i style="background:#abd9e9;height:2.4px"></i>较低需求</span>
        <span><i style="background:#ffffbf;height:3.6px;border:1px solid #d6c96a"></i>中需求</span>
        <span><i style="background:#fdae61;height:4.8px"></i>较高需求</span>
        <span><i style="background:#d7191c;height:6px"></i>高需求</span>
        <span><i class="zone-fill"></i>交通小区面</span>
        <span>OD 线：按需求分级</span>
        {od_demand_legend}
      </p>
      <div id="tna-map-od" class="map-box"></div>"""
        )
    else:
        parts.append(
            f'<p class="map-empty">{html.escape(_od_empty_reason((nb or {}).get("od") or {}))}</p>'
        )
    parts.append("</div></details>")
    parts.append("</div>")

    return {
        "html": "\n".join(parts),
        "js": MAP_INIT_JS if bundle_json else "",
        "css": MAP_EXTRA_CSS,
        "leaflet_head": leaflet_head_html(),
        "has_road": has_road,
        "has_od": has_od,
        "has_any": has_road or has_od,
    }
