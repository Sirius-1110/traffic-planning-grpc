"""路网流量图 / OD 期望线图 — gRPC 与 AI 报告共用渲染。"""
from __future__ import annotations

import html
import json
import os
from typing import Any

_DEFAULT_TDT_TK = os.environ.get("TNA_TIANDITU_TK", "2e48aa1ca86dd64106f2ae49459c7661")

MAP_EXTRA_CSS = """
.map-box{height:480px;width:100%;border:1px solid #dce4ec;border-radius:8px;z-index:1}
.map-box.compare{height:420px}
.map-stack{display:flex;flex-direction:column;gap:20px}
.map-subtitle{font-size:13px;font-weight:600;color:#1b4a82;margin:0 0 8px}
.map-legend{font-size:12px;color:#555;margin:8px 0 12px}
.map-legend span{display:inline-block;margin-right:14px}
.map-legend i{display:inline-block;width:22px;height:4px;vertical-align:middle;margin-right:4px}
.map-legend .od-w-thin{display:inline-block;width:28px;height:2px;background:#1a9850;vertical-align:middle;margin:0 4px}
.map-legend .od-w-mid{display:inline-block;width:28px;height:6px;background:#ffffbf;vertical-align:middle;margin:0 4px;border:1px solid #d6c85a;box-sizing:border-box}
.map-legend .od-w-thick{display:inline-block;width:28px;height:11px;background:#d73027;vertical-align:middle;margin:0 4px}
.map-legend .zone-fill{display:inline-block;width:14px;height:14px;background:rgba(52,152,219,0.5);border:1px solid #2980b9;vertical-align:middle;margin:0 4px}
.road-name-label,.od-pair-label,.zone-id-label{background:none!important;border:none!important}
.road-name-label span,.od-pair-label span,.zone-id-label span{
  display:inline-block;padding:1px 4px;border-radius:3px;font-size:10px;font-weight:600;line-height:1.2;
  white-space:nowrap;pointer-events:none;text-shadow:-1px -1px 2px #fff,1px -1px 2px #fff,-1px 1px 2px #fff,1px 1px 2px #fff}
.road-name-label span{color:#1a5276}
.od-pair-label span{color:#922b21;background:rgba(255,255,255,0.85)}
.zone-id-label span{color:#1f618d;background:rgba(255,255,255,0.8)}
.leaflet-od-legend{padding:8px 10px;background:rgba(255,255,255,0.92);border:1px solid #ccc;border-radius:6px;font-size:11px;line-height:1.6;box-shadow:0 1px 4px rgba(0,0,0,0.12)}
details.map-fold{margin-bottom:14px;border:1px solid #dce4ec;border-radius:8px;padding:10px 14px;background:#fafbfc}
details.map-fold>summary{cursor:pointer;font-size:14px;font-weight:600;color:#1b4a82;list-style-position:outside}
details.map-fold[open]>summary{margin-bottom:10px}
.map-empty{font-size:12px;color:#856404;line-height:1.8;padding:8px 4px}
.map-warn{font-size:13px;color:#7b241c;background:#fdecea;border:1px solid #f5b7b1;border-radius:6px;padding:10px 12px;margin:0 0 10px;line-height:1.65}
.map-fold-body{padding-top:4px}
.map-compare-grid{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-top:8px}
.map-compare-col h4{font-size:13px;font-weight:600;color:#1b4a82;margin:0 0 8px;text-align:center}
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
        {subdomains:'01234567', maxZoom:18, opacity:1, className:'tdt-muted'}
      );
    }
    var base = layer('vec'), anno = layer('cva');
    base.addTo(map); anno.addTo(map);
    base.on('tileerror', function() {
      if (map._geoqFallback) return;
      map._geoqFallback = true;
      try { map.removeLayer(base); map.removeLayer(anno); } catch(e) {}
      L.tileLayer('http://map.geoq.cn/ArcGIS/rest/services/ChinaOnlineStreetGray/MapServer/tile/{z}/{y}/{x}', {
        maxZoom:18, opacity:1, className:'tdt-muted'
      }).addTo(map);
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
  function rdYlGnReverse(cls) {
    var colors = ['#1a9850', '#91cf60', '#ffffbf', '#fc8d59', '#d73027'];
    return colors[Math.max(0, Math.min(4, cls || 0))];
  }
  function vcClass(v) {
    if (v == null) return -1;
    if (v < 0.2) return 0;
    if (v < 0.4) return 1;
    if (v < 0.6) return 2;
    if (v < 0.8) return 3;
    return 4;
  }
  function vcColor(v) {
    var cls = vcClass(v);
    return cls < 0 ? '#4a6fa5' : rdYlGnReverse(cls);
  }
  function roadLineStyle(v) {
    var cls = vcClass(v);
    if (cls < 0) {
      return { color: '#4a6fa5', weight: 2.8, opacity: 0.92 };
    }
    return { color: rdYlGnReverse(cls), weight: 3.2, opacity: 1 };
  }
  /** 绘制顺序：无 V/C 先画（底层），再按 V/C/流量从高到低，低饱和绿色最后绘制（顶层可见） */
  function roadLinksForDraw(links) {
    return (links || []).slice().sort(function(a, b) {
      var va = (a.v_c != null && isFinite(a.v_c)) ? Number(a.v_c) : null;
      var vb = (b.v_c != null && isFinite(b.v_c)) ? Number(b.v_c) : null;
      if (va == null && vb == null) return (a.link_id || 0) - (b.link_id || 0);
      if (va == null) return -1;
      if (vb == null) return 1;
      if (va !== vb) return vb - va;
      var fa = Number(a.volume || a.flow || 0);
      var fb = Number(b.volume || b.flow || 0);
      if (fa !== fb) return fb - fa;
      return (a.link_id || 0) - (b.link_id || 0);
    });
  }
  /** OD 期望线：高需求先画（底层），低需求后画（顶层，绿色在上） */
  function odLinesForDraw(lines) {
    return (lines || []).slice().sort(function(a, b) {
      var da = Number(a.demand || 0);
      var db = Number(b.demand || 0);
      if (da !== db) return db - da;
      return String(a.f_id || '') + '->' + String(a.t_id || '')
        > String(b.f_id || '') + '->' + String(b.t_id || '') ? 1 : -1;
    });
  }
  function demandBreaks(lines) {
    var vals = (lines || []).map(function(o) { return Number(o.demand || 0); })
      .filter(function(v) { return isFinite(v); }).sort(function(a,b){ return a-b; });
    if (!vals.length) return [];
    var breaks = [];
    for (var i = 1; i <= 4; i++) {
      breaks.push(vals[Math.min(vals.length - 1, Math.ceil(vals.length * i / 5) - 1)]);
    }
    return breaks;
  }
  function demandClass(demand, breaks) {
    var d = Number(demand || 0);
    for (var i = 0; i < breaks.length; i++) if (d <= breaks[i]) return i;
    return 4;
  }
  function odWeightByClass(cls) {
    return [2, 4, 6, 8, 10][Math.max(0, Math.min(4, cls || 0))];
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
        var tip = '小区 ' + z.area_id + (z.name ? (' ' + z.name) : '');
        L.polygon(ring, style).bindTooltip(tip, {sticky:true}).addTo(layer);
      });
    });
    layer.addTo(map);
    return layer;
  }
  function addRoadNameLabels(map, links) {
    var labelLayer = L.layerGroup().addTo(map);
    function refresh() {
      labelLayer.eachLayer(function(ly) { labelLayer.removeLayer(ly); });
      var z = map.getZoom();
      if (z < 12) return;
      var cap = z >= 15 ? 48 : (z >= 13 ? 28 : 12);
      var used = {}, shown = 0;
      var sorted = (links || []).slice().sort(function(a, b) {
        return (b.v_c || 0) - (a.v_c || 0);
      });
      for (var i = 0; i < sorted.length && shown < cap; i++) {
        var l = sorted[i];
        if (!l.latlng || l.latlng.length < 2) continue;
        var nm = (l.name || '').trim();
        if (!nm || nm.indexOf('路段') === 0) nm = '#' + l.link_id;
        else if (nm === '三亚') nm = nm + '\u00b7' + l.link_id;
        var mid = l.latlng[Math.floor(l.latlng.length / 2)];
        var key = Math.round(mid[0] * 800) + '_' + Math.round(mid[1] * 800) + '_' + nm;
        if (used[key]) continue;
        used[key] = 1;
        divLabel('road-name-label', nm).setLatLng(mid).addTo(labelLayer);
        shown++;
      }
    }
    map.on('zoomend moveend', refresh);
    refresh();
  }
  function drawRoadLayer(map, roadData, withLegend) {
    var latLngs = [];
    (roadData.centroid_connectors || []).forEach(function(c) {
      if (!c.latlng || c.latlng.length < 2) return;
      L.polyline(c.latlng, {
        color:'#7d3c98', weight:2.5, opacity:1, dashArray:'4 6', lineCap:'round'
      }).bindTooltip('质心连杆 type=10 \u00b7 小区' + c.zone_id, {sticky:true}).addTo(map);
      c.latlng.forEach(function(p) { latLngs.push(p); });
    });
    (roadLinksForDraw(roadData.links)).forEach(function(l) {
      if (!l.latlng || l.latlng.length < 2) return;
      var tip = (l.name || ('路段' + l.link_id)) + ' #' + l.link_id;
      if (l.v_c != null) tip += ' | V/C=' + l.v_c;
      else tip += ' | 无分配流量';
      var st = roadLineStyle(l.v_c);
      L.polyline(l.latlng, {
        color: st.color, weight: st.weight, opacity: st.opacity, lineCap: 'round', lineJoin: 'round'
      }).bindTooltip(tip, {sticky:true, direction:'top', opacity:0.95}).addTo(map);
      l.latlng.forEach(function(p) { latLngs.push(p); });
    });
    addRoadNameLabels(map, roadData.links);
    if (withLegend !== false) {
      var roadLegend = L.control({position:'bottomright'});
      roadLegend.onAdd = function() {
        var div = L.DomUtil.create('div', 'leaflet-od-legend');
        div.innerHTML = '<b>路网图例</b><br/>'
          + '<span style="display:inline-block;width:34px;height:4px;background:#1a9850;vertical-align:middle"></span> V/C 0-0.2<br/>'
          + '<span style="display:inline-block;width:34px;height:4px;background:#91cf60;vertical-align:middle"></span> V/C 0.2-0.4<br/>'
          + '<span style="display:inline-block;width:34px;height:4px;background:#ffffbf;vertical-align:middle"></span> V/C 0.4-0.6<br/>'
          + '<span style="display:inline-block;width:34px;height:4px;background:#fc8d59;vertical-align:middle"></span> V/C 0.6-0.8<br/>'
          + '<span style="display:inline-block;width:34px;height:4px;background:#d73027;vertical-align:middle"></span> V/C 0.8-max<br/>'
          + '<span style="display:inline-block;width:34px;height:3px;background:#4a6fa5;vertical-align:middle"></span> 无分配流量<br/>'
          + '<span style="display:inline-block;width:34px;height:3px;background:#7d3c98;vertical-align:middle;border-top:1px dashed #7d3c98"></span> 质心连杆<br/>'
          + '<span style="color:#1a5276">\u25a0</span> 路段标注';
        return div;
      };
      roadLegend.addTo(map);
    }
    fitMap(map, roadData.bounds, latLngs, roadData.center);
  }
  function mountRoadMap(elId, roadData, withLegend) {
    var el = document.getElementById(elId);
    if (!el || !roadData || !roadData.links || !roadData.links.length) return null;
    el.innerHTML = '';
    var map = L.map(elId, {zoomControl:true, attributionControl:true});
    addTdt(map, roadData.tdtTk || '');
    drawRoadLayer(map, roadData, withLegend);
    setTimeout(function() { map.invalidateSize(); }, 280);
    return map;
  }
  function drawOdMap(odData) {
    if (!document.getElementById('tna-map-od') || !odData.od_lines || !odData.od_lines.length) return;
    var omap = L.map('tna-map-od', {zoomControl:true, attributionControl:true});
    addTdt(omap, odData.tdtTk || '');
    var minD = odData.demand_min || 1, maxD = odData.demand_max || 1, latLngs = [];
    var odBreaks = demandBreaks(odData.od_lines || []);
    var zoneLayer = L.layerGroup();
    var odLayer = L.layerGroup();
    var labelLayer = L.layerGroup();
    drawZonePolygons(omap, odData.zone_polygons, {
      color:'#2471a3', weight:1, opacity:1, fillColor:'#5dade2', fillOpacity:0.5
    });
    (odData.zone_points || []).forEach(function(z) {
      if (!z.latlng) return;
      L.circleMarker(z.latlng, {
        radius:6, color:'#1a5276', fillColor:'#2980b9', fillOpacity:0.9, weight:2
      }).bindTooltip('小区 ' + z.area_id, {sticky:true}).addTo(zoneLayer);
      divLabel('zone-id-label', String(z.area_id)).setLatLng(z.latlng).addTo(labelLayer);
      latLngs.push(z.latlng);
    });
    odLinesForDraw(odData.od_lines || []).forEach(function(o) {
      var ll = (o.coordinates || []).map(function(c) { return [c[1], c[0]]; });
      if (ll.length < 2) return;
      var cls = demandClass(o.demand, odBreaks);
      var w = odWeightByClass(cls);
      L.polyline(ll, {
        color: rdYlGnReverse(cls), weight:w, opacity:1, lineCap:'round', lineJoin:'round'
      }).bindTooltip('O' + o.f_id + '\u2192D' + o.t_id + ' \u9700\u6c42=' + o.demand, {sticky:true}).addTo(odLayer);
      var mid = lineMid(ll);
      if (mid) divLabel('od-pair-label', o.f_id + '\u2192' + o.t_id).setLatLng(mid).addTo(labelLayer);
      ll.forEach(function(p) { latLngs.push(p); });
    });
    zoneLayer.addTo(omap); odLayer.addTo(omap); labelLayer.addTo(omap);
    var odLegend = L.control({position:'bottomright'});
    odLegend.onAdd = function() {
      var div = L.DomUtil.create('div', 'leaflet-od-legend');
      div.innerHTML = '<b>OD \u56fe\u4f8b</b><br/>'
        + '<span style="display:inline-block;width:28px;height:2px;background:#1a9850;vertical-align:middle"></span> \u4f4e\u9700\u6c42<br/>'
        + '<span style="display:inline-block;width:28px;height:6px;background:#ffffbf;vertical-align:middle"></span> \u4e2d\u9700\u6c42<br/>'
        + '<span style="display:inline-block;width:28px;height:10px;background:#d73027;vertical-align:middle"></span> \u9ad8\u9700\u6c42<br/>'
        + '<span style="display:inline-block;width:12px;height:12px;background:rgba(93,173,226,0.5);border:1px solid #2471a3;vertical-align:middle"></span> \u5c0f\u533a\u9762<br/>'
        + '<span style="color:#1f618d">\u25a0</span> \u5c0f\u533a ID<br/>'
        + '<span style="color:#922b21">\u25a0</span> O\u2192D \u6807\u6ce8<br/>'
        + '\u9700\u6c42 ' + minD + ' - ' + maxD;
      return div;
    };
    odLegend.addTo(omap);
    fitMap(omap, odData.bounds, latLngs, odData.center);
    setTimeout(function() { omap.invalidateSize(); }, 350);
  }
  function initTnaMaps() {
    try {
      if (typeof L === 'undefined') {
        showMapError('tna-map-road', 'Leaflet \u5e93\u672a\u52a0\u8f7d\uff08\u8bf7\u68c0\u67e5\u7f51\u7edc\uff09');
        showMapError('tna-map-od', 'Leaflet \u5e93\u672a\u52a0\u8f7d\uff08\u8bf7\u68c0\u67e5\u7f51\u7edc\uff09');
        return;
      }
      var raw = document.getElementById('tna-map-bundle');
      if (!raw) return;
      var bundle = JSON.parse(raw.textContent);
      _roadMaps.forEach(function(m) { try { m.remove(); } catch(e) {} });
      _roadMaps = [];
      if (bundle.mode === 'compare') {
        (bundle.layers || []).forEach(function(layer, idx) {
          var m = mountRoadMap('tna-map-road-' + layer.key, layer.road, idx === 0);
          if (m) _roadMaps.push(m);
        });
      } else if (bundle.road && bundle.road.links && bundle.road.links.length) {
        var m = mountRoadMap('tna-map-road', bundle.road, true);
        if (m) _roadMaps.push(m);
      }
      if (bundle.od && bundle.od.od_lines && bundle.od.od_lines.length) {
        drawOdMap(bundle.od);
      }
    } catch (e) {
      console.error('TNA map init failed', e);
      showMapError('tna-map-road', '\u5730\u56fe\u6e32\u67d3\u5931\u8d25: ' + e.message);
      showMapError('tna-map-od', '\u5730\u56fe\u6e32\u67d3\u5931\u8d25: ' + e.message);
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
    if assign.get("truncate_note"):
        return str(assign["truncate_note"])
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
) -> dict[str, Any]:
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


def _pack_od_layer(nb: dict[str, Any]) -> dict[str, Any]:
    mmap = nb.get("map") or {}
    od = nb.get("od") or {}
    centroid = nb.get("centroid") or {}
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
        <span><i style="background:#4a6fa5"></i>无分配流量路段</span>
        <span><i style="background:#7d3c98;height:3px"></i>质心连杆 type=10</span>
      </p>"""
    return """
      <p class="map-legend">
        <span><strong>饱和度 V/C：</strong></span>
        <span><i style="background:#1a9850"></i>0-0.2 畅通</span>
        <span><i style="background:#91cf60"></i>0.2-0.4 基本畅通</span>
        <span><i style="background:#ffffbf"></i>0.4-0.6 轻度</span>
        <span><i style="background:#fc8d59"></i>0.6-0.8 中度</span>
        <span><i style="background:#d73027"></i>0.8-max 高饱和</span>
        <span><i style="background:#4a6fa5"></i>无分配流量</span>
        <span><i style="background:#7d3c98;height:3px"></i>质心连杆 type=10</span>
        <span>路段线宽 3.2，透明度 100%；底图淡化显示</span>
      </p>"""


def build_map_bundle_payload(
    nb: dict[str, Any] | None,
    *,
    pipeline: dict[str, Any] | None = None,
    compare_layers: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """构建前端 tna-map-bundle JSON。"""
    if compare_layers:
        layers_out = []
        od_src = None
        for i, layer in enumerate(compare_layers):
            bundle = layer.get("bundle") or {}
            pl = layer.get("pipeline") or pipeline
            road = _pack_road_layer(bundle, pipeline=pl)
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
            "tdtTk": _DEFAULT_TDT_TK,
            "layers": layers_out,
        }
        if od_src:
            payload["od"] = _pack_od_layer(od_src)
        return payload

    nb = nb or {}
    mmap = nb.get("map") or {}
    od = nb.get("od") or {}
    has_road = bool(mmap.get("ok") and mmap.get("links"))
    has_od = bool((od.get("lines") or []))
    if not has_road and not has_od:
        return {}
    payload = {
        "mode": "single",
        "tdtTk": _DEFAULT_TDT_TK,
        "road": _pack_road_layer(nb, pipeline=pipeline),
    }
    if has_od:
        payload["od"] = _pack_od_layer(nb)
    return payload


def render_map_sections(
    nb: dict[str, Any] | None,
    *,
    pipeline: dict[str, Any] | None = None,
    compare_layers: list[dict[str, Any]] | None = None,
    section_title: str = "空间分布：路网流量与 OD 期望线",
) -> dict[str, Any]:
    """
    返回路网/OD 折叠区块 HTML、初始化 JS、Leaflet head。
    compare_layers: [{key, label, case_id, bundle, pipeline}, ...] 用于方案对比双图。
    """
    bundle_payload = build_map_bundle_payload(
        nb, pipeline=pipeline, compare_layers=compare_layers
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
    ]
    if is_compare:
        parts.append(
            '<p class="desc" style="margin-bottom:14px">'
            "并排对比基础方案与本方案路网饱和度（V/C 分级着色）；"
            "重叠路段按 V/C 从低到高叠放（低饱和绿色在上、高饱和红色在下）；"
            "OD 期望线按需求等数量分级，低需求线在上；底图采用天地图淡化样式。"
            "无数据的小节默认折叠并说明原因。</p>"
        )
    else:
        parts.append(
            '<p class="desc" style="margin-bottom:14px">'
            "机动车路段按饱和度分级着色，重叠处低 V/C（绿色）在上、高 V/C（红色）在下；"
            "OD 期望线低需求在上；底图采用天地图淡化样式。无数据的小节默认折叠并说明原因。</p>"
        )
    if bundle_json:
        parts.append(f'<script type="application/json" id="tna-map-bundle">{bundle_json}</script>')

    road_open = " open" if has_road else ""
    if is_compare:
        road_summary = "机动车路网流量分布对比（饱和度着色）"
    elif has_road and has_vc_any:
        road_summary = "机动车路网流量分布（饱和度着色）"
    elif has_road:
        road_summary = "机动车路网结构（无分配流量，仅拓扑）"
    else:
        mmap0 = (nb or {}).get("map") or {}
        road_summary = f"机动车路网（暂无：{html.escape(_road_empty_reason(mmap0))}）"

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
        parts.append('<div id="tna-map-road" class="map-box"></div>')
    else:
        mmap0 = (nb or {}).get("map") or {}
        parts.append(f'<p class="map-empty">{html.escape(_road_empty_reason(mmap0))}</p>')
    parts.append("</div></details>")

    od_open = " open" if has_od else ""
    od_summary = (
        f"OD 期望线分布（Top {len(od_lines)} 对，RdYlGn 反转）"
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
        <span><i class="od-w-thin"></i>低需求 / 2</span>
        <span><i class="od-w-mid"></i>中需求 / 6</span>
        <span><i class="od-w-thick"></i>高需求 / 10</span>
        <span><i class="zone-fill"></i>交通小区面（框线1，填充50%）</span>
        <span>标注：小区 ID / O→D</span>
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
