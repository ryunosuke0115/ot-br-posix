
/* ============================================================
 * 設定
 * ============================================================ */

const API_URL = '/api/network-info';
const DEVICE_API_URL = '/api/device-metadata';
const POLL_INTERVAL_MS = 2000;
const NODE_VIEWPORT_PADDING = 48;


/* ============================================================
 * トラフィック表示用スケール
 * ============================================================ */

const auraRadiusScale =
  d3.scaleLog()
    .domain([1, 60])
    .range([4, 46])
    .clamp(true);

const auraOpacityScale =
  d3.scaleLog()
    .domain([1, 60])
    .range([0.08, 0.5])
    .clamp(true);


/* ============================================================
 * グローバル状態
 * ============================================================ */

let simulation = null;

let svg;
let zoomLayer;
let linkLayer;
let nodeLayer;
let graphWidth = 0;
let graphHeight = 0;
let displayedGraph = null;

let selectedNodeId = null;

let deviceMetadataByEui64 = new Map();
let lastNetworkInfo = null;

/*
 * 現在画面に表示しているノード情報
 *
 * rloc16 -> nodes[] 内の生データ
 */
let latestNodesById = new Map();

/*
 * 前回取得したネットワーク情報
 *
 * topology:
 *   ノード・リンクのトポロジ情報
 *
 * traffic:
 *   各ノードのトラフィック情報
 */
let previousTopologyKey = null;
let previousTrafficById = new Map();


/* ============================================================
 * 初期化
 * ============================================================ */

function initSvg() {

  const pane = document.querySelector('.graph-pane');

  graphWidth = pane.clientWidth;
  graphHeight = pane.clientHeight;

  svg = d3.select('#graph')
    .attr('viewBox', [0, 0, graphWidth, graphHeight]);

  zoomLayer = svg.append('g');

  svg.call(
    d3.zoom()
      .scaleExtent([0.4, 3])
      .on('zoom', (event) => {
        zoomLayer.attr('transform', event.transform);
      })
  );

  linkLayer =
    zoomLayer
      .append('g')
      .attr('class', 'links');

  nodeLayer =
    zoomLayer
      .append('g')
      .attr('class', 'nodes');

  simulation =
    d3.forceSimulation()
      .force(
        'link',
        d3.forceLink()
          .id(node => node.id)
          .distance(120)
          .strength(0.45)
      )
      .force(
        'charge',
        d3.forceManyBody()
          .strength(-260)
      )
      .force(
        'center',
        d3.forceCenter(graphWidth / 2, graphHeight / 2)
      )
      .force(
        'x',
        d3.forceX(graphWidth / 2)
          .strength(0.03)
      )
      .force(
        'y',
        d3.forceY(graphHeight / 2)
          .strength(0.03)
      )
      .force(
        'collision',
        d3.forceCollide()
          .radius(48)
      );
}

function constrainNodePosition(node) {
  const maximumX = Math.max(NODE_VIEWPORT_PADDING, graphWidth - NODE_VIEWPORT_PADDING);
  const maximumY = Math.max(NODE_VIEWPORT_PADDING, graphHeight - NODE_VIEWPORT_PADDING);

  if (node.x <= NODE_VIEWPORT_PADDING && node.vx < 0) {
    node.vx = 0;
  }

  if (node.x >= maximumX && node.vx > 0) {
    node.vx = 0;
  }

  if (node.y <= NODE_VIEWPORT_PADDING && node.vy < 0) {
    node.vy = 0;
  }

  if (node.y >= maximumY && node.vy > 0) {
    node.vy = 0;
  }

  node.x = Math.max(NODE_VIEWPORT_PADDING, Math.min(maximumX, node.x));
  node.y = Math.max(NODE_VIEWPORT_PADDING, Math.min(maximumY, node.y));
}

function nodeSeed(id) {
  return Array.from(id).reduce((value, character) => (value * 31 + character.charCodeAt(0)) >>> 0, 0);
}

function prepareTopologySimulation(graph) {
  const previousNodes = new Map(
    (displayedGraph ? displayedGraph.nodes : []).map(node => [node.id, node])
  );
  const nodesById = new Map(graph.nodes.map(node => [node.id, node]));

  graph.nodes.forEach(node => {
    const previousNode = previousNodes.get(node.id);

    if (previousNode && Number.isFinite(previousNode.x) && Number.isFinite(previousNode.y)) {
      node.x = previousNode.x;
      node.y = previousNode.y;
      node.vx = previousNode.vx || 0;
      node.vy = previousNode.vy || 0;
      return;
    }

    const angle = (nodeSeed(node.id) % 360) * Math.PI / 180;
    const radius = 60 + (nodeSeed(`${node.id}:radius`) % 90);

    node.x = graphWidth / 2 + radius * Math.cos(angle);
    node.y = graphHeight / 2 + radius * Math.sin(angle);
    constrainNodePosition(node);
  });

  graph.links.forEach(link => {
    const sourceId = typeof link.source === 'object' ? link.source.id : link.source;
    const targetId = typeof link.target === 'object' ? link.target.id : link.target;

    link.source = nodesById.get(sourceId);
    link.target = nodesById.get(targetId);
  });
}

function renderTopologyPositions(graph) {
  linkLayer
    .selectAll('line.link')
    .attr('x1', link => link.source.x)
    .attr('y1', link => link.source.y)
    .attr('x2', link => link.target.x)
    .attr('y2', link => link.target.y);

  nodeLayer
    .selectAll('g.node')
    .attr('transform', node => `translate(${node.x},${node.y})`);
}

function resizeGraph() {
  const pane = document.querySelector('.graph-pane');

  graphWidth = pane.clientWidth;
  graphHeight = pane.clientHeight;

  svg.attr('viewBox', [0, 0, graphWidth, graphHeight]);

  if (displayedGraph) {
    displayedGraph.nodes.forEach(constrainNodePosition);
    renderTopologyPositions(displayedGraph);
    simulation.force('center', d3.forceCenter(graphWidth / 2, graphHeight / 2));
    simulation.force('x', d3.forceX(graphWidth / 2).strength(0.03));
    simulation.force('y', d3.forceY(graphHeight / 2).strength(0.03));
    simulation.alpha(0.2).restart();
  }
}


/* ============================================================
 * トラフィック総量
 * ============================================================ */

function nodeTrafficTotal(traffic) {

  if (!traffic) {
    return 0;
  }

  const out =
    (traffic['thread-to-external'] || [])
      .reduce(
        (s, t) =>
          s + (t['bytes-per-sec'] || 0),
        0
      );

  const inn =
    (traffic['external-to-thread'] || [])
      .reduce(
        (s, t) =>
          s + (t['bytes-per-sec'] || 0),
        0
      );

  return out + inn;
}


/* ============================================================
 * JSONを安定した文字列に変換
 *
 * オブジェクトのキー順序による差を無視するため、
 * 再帰的にキーをソートする。
 * ============================================================ */

function stableStringify(value) {

  if (value === null || value === undefined) {
    return String(value);
  }

  if (typeof value !== 'object') {
    return JSON.stringify(value);
  }

  if (Array.isArray(value)) {
    return '[' +
      value
        .map(item => stableStringify(item))
        .join(',') +
      ']';
  }

  return '{' +
    Object.keys(value)
      .sort()
      .map(
        key =>
          JSON.stringify(key) +
          ':' +
          stableStringify(value[key])
      )
      .join(',') +
    '}';
}


/* ============================================================
 * データ変換
 *
 * /api/network-info
 *       ↓
 * D3用グラフデータ
 * ============================================================ */

function parseNetworkInfo(data) {

  const nodes = [];

  const linksSet = new Map();

  const byId = new Map();

  /*
   * rloc16 -> nodes[] 内の生データ
   */
  const rawByRloc = new Map();

  data.nodes.forEach(router => {
    rawByRloc.set(router.rloc16, router);
  });


  /*
   * children[] に登場する rloc16
   */
  const childRlocSet = new Set();

  data.nodes.forEach(router => {

    (router.children || [])
      .forEach(child => {
        childRlocSet.add(child.rloc16);
      });

  });


  /*
   * ノード・リンクを構築
   */

  data.nodes.forEach(router => {

    /*
     * 他の Router の children[] に存在する場合、
     * トポロジ上は Child。
     *
     * Child は後の children[] 処理で追加する。
     */
    if (childRlocSet.has(router.rloc16)) {
      return;
    }


    /*
     * Leader判定
     */

    const isLeader =
      router['leader-data'] &&
      (
        parseInt(router.rloc16, 16) >> 10
      ) ===
      router['leader-data']['leader-router-id'];


    /*
     * Router / Leader
     */

    const rNode = {

      id: router.rloc16,

      label:
        router.eui64 &&
        deviceMetadataByEui64.has(router.eui64.toLowerCase())
          ? deviceMetadataByEui64.get(router.eui64.toLowerCase()).name
          : router.rloc16,

      kind:
        isLeader
          ? 'leader'
          : 'router',

      traffic:
        router.traffic,

      trafficTotal:
        nodeTrafficTotal(
          router.traffic
        ),

      raw:
        router
    };

    nodes.push(rNode);

    byId.set(
      router.rloc16,
      router
    );


    /*
     * Router間リンク
     */

    (router.routes || [])
      .forEach(route => {

        const a = router.rloc16;
        const b = route.rloc16;

        /*
         * 自己ループ除外
         */
        if (a === b) {
          return;
        }

        /*
         * Childとのリンクはここでは作らない
         */
        if (childRlocSet.has(b)) {
          return;
        }

        const key =
          [a, b]
            .sort()
            .join('|');

        if (!linksSet.has(key)) {

          linksSet.set(
            key,
            {
              source: a,
              target: b,
              kind: 'mesh'
            }
          );

        }

      });


    /*
     * Childノード
     */

    (router.children || [])
      .forEach(child => {

        const observedChild =
          rawByRloc.get(
            child.rloc16
          );

        const childTraffic =
          observedChild
            ? observedChild.traffic
            : null;

        const childRaw = {
          ...child,
          parentRloc16: router.rloc16
        };


        nodes.push({

          id:
            child.rloc16,

          label:
            child.rloc16,

          kind:
            'child',

          traffic:
            childTraffic,

          trafficTotal:
            nodeTrafficTotal(
              childTraffic
            ),

          parentId:
            router.rloc16,

          raw:
            child

        });


        byId.set(
          child.rloc16,
          childRaw
        );


        const key =
          [router.rloc16, child.rloc16]
            .sort()
            .join('|');


        linksSet.set(
          key,
          {
            source:
              router.rloc16,

            target:
              child.rloc16,

            kind:
              'child'
          }
        );

      });

  });


  return {

    nodes,

    links:
      Array.from(
        linksSet.values()
      ),

    byId

  };
}


/* ============================================================
 * トポロジの比較用キー
 *
 * 位置 x/y は含めない。
 * APIから取得したデータそのものから、
 * ノードの種類・存在・リンク関係だけを比較する。
 * ============================================================ */

function createTopologyKey(graph) {

  const nodes =
    graph.nodes
      .map(node => ({
        id: node.id,
        kind: node.kind,
        parentId: node.parentId || null
      }))
      .sort(
        (a, b) =>
          a.id.localeCompare(b.id)
      );


  const links =
    graph.links
      .map(link => ({
        source:
          typeof link.source === 'object'
            ? link.source.id
            : link.source,

        target:
          typeof link.target === 'object'
            ? link.target.id
            : link.target,

        kind:
          link.kind
      }))
      .map(link => ({
        ...link,

        source:
          link.source < link.target
            ? link.source
            : link.target,

        target:
          link.source < link.target
            ? link.target
            : link.source
      }))
      .sort(
        (a, b) => {

          const aKey =
            `${a.source}|${a.target}|${a.kind}`;

          const bKey =
            `${b.source}|${b.target}|${b.kind}`;

          return aKey.localeCompare(bKey);
        }
      );


  return stableStringify({
    nodes,
    links
  });
}


/* ============================================================
 * トラフィック情報のキー
 *
 * 各ノードごとに独立して比較する。
 * ============================================================ */

function createTrafficKey(traffic) {

  if (!traffic) {
    return stableStringify(null);
  }

  return stableStringify({

    'thread-to-external':
      traffic['thread-to-external'] || [],

    'external-to-thread':
      traffic['external-to-thread'] || []

  });
}


/* ============================================================
 * ノードのトラフィック表示だけ更新
 *
 * トポロジが変化していなくても、
 * trafficだけ変化した場合はこちらを使用する。
 *
 * ノード座標は変更しない。
 * ============================================================ */

function updateNodeTraffic(nodeData) {

  const nodeSelection =
    nodeLayer
      .selectAll('g.node')
      .filter(
        d => d.id === nodeData.id
      );


  /*
   * データを更新
   */

  const displayedNode = nodeSelection.datum();

  if (displayedNode) {
    const {x, y, vx, vy} = displayedNode;

    Object.assign(displayedNode, nodeData, {x, y, vx, vy});
  }


  /*
   * trafficTotalに応じて
   * has-traffic クラスを更新
   */

  nodeSelection
    .classed(
      'has-traffic',
      nodeData.trafficTotal > 0.5
    );


  /*
   * オーラだけ更新
   */

  nodeSelection
    .select('circle.aura')
    .interrupt()
    .transition()
    .duration(300)
    .attr(
      'r',
      (
        nodeData.kind === 'child'
          ? 13
          : nodeData.kind === 'leader'
            ? 22
            : 19
      ) +
      auraRadiusScale(
        Math.max(
          nodeData.trafficTotal,
          0.01
        )
      )
    )
    .attr(
      'opacity',
      nodeData.trafficTotal > 0.5
        ? auraOpacityScale(
            nodeData.trafficTotal
          )
        : 0
    );


  /*
   * 選択中ノードなら詳細パネルも更新
   */

  if (
    selectedNodeId === nodeData.id
    && !shouldPreserveDeviceForm()
  ) {
    renderDetail(nodeData.id);
  }
}


/* ============================================================
 * トポロジ更新
 *
 * トポロジが変化した場合のみ実行する。
 *
 * 既存座標を引き継いで力学シミュレーションを更新する。
 * ============================================================ */

function updateTopology(graph) {

  /*
   * 最新ノード情報を保存
   */

  latestNodesById =
    graph.byId;

  prepareTopologySimulation(graph);
  displayedGraph = graph;


  /* ----------------------------------------------------------
   * リンク
   * ---------------------------------------------------------- */

  const link =
    linkLayer
      .selectAll('line.link')
      .data(
        graph.links,
        d =>
          [
            d.source.id || d.source,
            d.target.id || d.target
          ]
            .sort()
            .join('|')
      );


  link
    .exit()
    .remove();


  const linkEnter =
    link
      .enter()
      .append('line')
      .attr(
        'class',
        d =>
          'link' +
          (
            d.kind === 'child'
              ? ' child-link'
              : ''
          )
      );


  linkEnter
    .merge(link)
    .attr(
      'class',
      d =>
        'link' +
        (
          d.kind === 'child'
            ? ' child-link'
            : ''
        )
    );


  /* ----------------------------------------------------------
   * ノード
   * ---------------------------------------------------------- */

  const node =
    nodeLayer
      .selectAll('g.node')
      .data(
        graph.nodes,
        d => d.id
      );


  node
    .exit()
    .remove();


  const nodeEnter =
    node
      .enter()
      .append('g')
      .attr(
        'class',
        d =>
          'node ' +
          d.kind
      )
      .on(
        'click',
        (event, d) =>
          selectNode(d.id)
      );


  nodeEnter
    .append('circle')
    .attr(
      'class',
      'aura'
    )
    .attr(
      'fill',
      '#d85a30'
    )
    .attr(
      'stroke',
      'none'
    );


  nodeEnter
    .append('circle')
    .attr(
      'class',
      'body'
    );

  nodeEnter
    .append('text')
    .attr(
      'class',
      'node-label'
    )
    .attr(
      'y',
      34
    );


  const nodeMerged =
    nodeEnter.merge(node);


  /*
   * ノードのクラス
   */

  nodeMerged
    .attr(
      'class',
      d => {

        let cls =
          'node ' +
          d.kind;

        if (
          d.trafficTotal > 0.5
        ) {
          cls +=
            ' has-traffic';
        }

        if (
          d.id === selectedNodeId
        ) {
          cls +=
            ' selected';
        }

        return cls;
      }
    );


  /*
   * ノード本体の大きさ
   */

  nodeMerged
    .select('circle.body')
    .attr(
      'r',
      d =>
        d.kind === 'child'
          ? 13
          : d.kind === 'leader'
            ? 22
            : 19
    );

  nodeMerged
    .select('text.node-label')
    .text(
      d => d.label
    );


  /*
   * トラフィックのオーラ
   */

  nodeMerged
    .select('circle.aura')
    .attr(
      'r',
      d =>
        (
          d.kind === 'child'
            ? 13
            : d.kind === 'leader'
              ? 22
              : 19
        ) +
        auraRadiusScale(
          Math.max(
            d.trafficTotal,
            0.01
          )
        )
    )
    .attr(
      'opacity',
      d =>
        d.trafficTotal > 0.5
          ? auraOpacityScale(
              d.trafficTotal
            )
          : 0
    );


  renderTopologyPositions(graph);

  simulation
    .nodes(graph.nodes)
    .on('tick', () => {
      graph.nodes.forEach(constrainNodePosition);
      renderTopologyPositions(graph);
    });

  simulation
    .force('link')
    .links(graph.links);

  simulation
    .alpha(0.35)
    .restart();


  /*
   * トラフィック情報の状態も更新
   */

  previousTrafficById.clear();

  graph.nodes.forEach(nodeData => {

    previousTrafficById.set(
      nodeData.id,
      createTrafficKey(
        nodeData.traffic
      )
    );

  });


  /*
   * 選択中ノードの詳細
   */

  if (selectedNodeId) {
    if (!shouldPreserveDeviceForm()) {
      renderDetail(
        selectedNodeId
      );
    }
  }
}


/* ============================================================
 * トラフィックだけ更新
 *
 * トポロジが変わっていない場合はこちら。
 * ============================================================ */

function updateTrafficOnly(graph) {

  /*
   * latestNodesByIdを更新
   */

  latestNodesById =
    graph.byId;


  /*
   * 管理情報によるラベル変更は、トポロジを再構築せずに反映する。
   */

  graph.nodes.forEach(nodeData => {
    nodeLayer
      .selectAll('g.node')
      .filter(d => d.id === nodeData.id)
      .select('text.node-label')
      .text(nodeData.label);
  });


  graph.nodes.forEach(nodeData => {

    const newKey =
      createTrafficKey(
        nodeData.traffic
      );

    const oldKey =
      previousTrafficById.get(
        nodeData.id
      );


    /*
     * 変化していないノードは何もしない
     */

    if (
      newKey === oldKey
    ) {
      return;
    }


    /*
     * 変化したノードだけ更新
     */

    updateNodeTraffic(
      nodeData
    );


    /*
     * 比較用状態を更新
     */

    previousTrafficById.set(
      nodeData.id,
      newKey
    );

  });


  /*
   * ノードが削除された場合、
   * 比較用Mapからも削除する。
   *
   * 通常はトポロジ変更として処理されるため、
   * ここに来ることはほぼない。
   */

  for (
    const id of previousTrafficById.keys()
  ) {

    if (
      !graph.byId.has(id)
    ) {

      previousTrafficById.delete(id);

    }

  }
}


/* ============================================================
 * グラフ更新の入口
 *
 * ここで
 *
 *   トポロジ変更
 *       ↓
 *   updateTopology()
 *
 *   トポロジ不変
 *       ↓
 *   updateTrafficOnly()
 *
 * に分岐する。
 * ============================================================ */

function updateGraph(graph) {

  const newTopologyKey =
    createTopologyKey(
      graph
    );


  /*
   * 初回
   */

  if (
    previousTopologyKey === null
  ) {

    previousTopologyKey =
      newTopologyKey;

    updateTopology(
      graph
    );

    return;
  }


  /*
   * トポロジが変わった
   */

  if (
    newTopologyKey !==
    previousTopologyKey
  ) {

    previousTopologyKey =
      newTopologyKey;

    updateTopology(
      graph
    );

    return;
  }


  /*
   * トポロジは変わっていない
   *
   * trafficだけ比較
   */

  updateTrafficOnly(
    graph
  );
}


/* ============================================================
 * 詳細パネル
 * ============================================================ */

function selectNode(id) {

  selectedNodeId =
    id;

  d3.selectAll('g.node')
    .classed(
      'selected',
      d => d.id === id
    );


  renderDetail(id);
}


/* ============================================================
 * Traffic一覧HTML
 * ============================================================ */

function trafficRowsHtml(
  entries,
  addrKey,
  activeClass
) {

  if (
    !entries ||
    entries.length === 0
  ) {

    return `
      <div class="traffic-empty">
        通信なし
      </div>
    `;
  }


  return entries
    .map(e => {

      const bps =
        e['bytes-per-sec'] || 0;


      const isMulticast =
        (e[addrKey] || '')
          .startsWith('ff02::');


      const rowClass =
        (
          !isMulticast &&
          bps > 0.5
        )
          ? ` ${activeClass}`
          : '';


      return `
        <div class="traffic-row${rowClass}">
          <span class="addr">
            ${e[addrKey]}
          </span>

          <span class="rate">
            ${bps.toFixed(1)} B/s
          </span>
        </div>
      `;

    })
    .join('');
}


/* ============================================================
 * 詳細パネル表示
 * ============================================================ */

function renderDetail(id) {

  const raw =
    latestNodesById.get(id);

  const pane =
    document.getElementById(
      'detail-pane'
    );


  if (!raw) {

    pane.innerHTML = `
      <p class="detail-label">
        選択中
      </p>

      <p class="detail-id">
        ${id}
      </p>

      <p style="font-size:12px;color:var(--text-muted);">
        このノードのトラフィック情報は取得できませんでした。
      </p>
    `;

    return;
  }

  const isRouter = Boolean(raw.eui64);
  const metadata =
    isRouter
      ? deviceMetadataByEui64.get(raw.eui64.toLowerCase())
      : null;


  const traffic =
    raw.traffic || {
      'thread-to-external': [],
      'external-to-thread': []
    };


  const identityHtml =
    isRouter
      ? `
        <dl class="metadata-list">
          <dt>RLOC16</dt><dd>${escapeHtml(raw.rloc16 || id)}</dd>
        </dl>

        ${
          metadata
            ? `
              <div id="device-metadata-summary">
                <dl class="metadata-list device-metadata">
                  <dt>名前</dt><dd>${escapeHtml(metadata.name)}</dd>
                  <dt>場所</dt><dd>${escapeHtml(metadata.location || '')}</dd>
                  <dt>種別</dt><dd>${escapeHtml(metadata.type || '')}</dd>
                </dl>
                <button class="device-edit-button" id="device-edit-button" type="button">変更</button>
                <button class="ipv6-toggle-button" id="ipv6-toggle-button" type="button" aria-expanded="false">▼ IPv6 アドレス</button>
                <div id="ipv6-addresses" hidden>
                  ${ipv6AddressesHtml(raw['ipv6-lists'])}
                </div>
              </div>
            `
            : `
              <form class="device-form" id="device-form">
                <label>名前（必須）
                  <input id="device-name" required value="">
                </label>
                <label>場所
                  <input id="device-location" value="">
                </label>
                <label>種別
                  <input id="device-type" value="">
                </label>
                <button type="submit">保存</button>
                <p class="device-form-status" id="device-form-status"></p>
              </form>
            `
        }
      `
      : `
        <dl class="metadata-list">
          <dt>種別</dt><dd>Child</dd>
          <dt>親 Router</dt><dd>${escapeHtml(raw.parentRloc16 || '')}</dd>
          <dt>RLOC16</dt><dd>${escapeHtml(raw.rloc16 || id)}</dd>
        </dl>
      `;

  pane.innerHTML = `
    <p class="detail-label">
      選択中
    </p>

    <p class="detail-id">
      ${escapeHtml(isRouter && metadata ? metadata.name : id)}
    </p>

    ${identityHtml}

    <p class="section-heading">
      Thread → External
    </p>

    <div class="traffic-list">
      ${
        trafficRowsHtml(
          traffic['thread-to-external'],
          'dst-ipv6',
          'active-out'
        )
      }
    </div>

    <p class="section-heading">
      External → Thread
    </p>

    <div class="traffic-list">
      ${
        trafficRowsHtml(
          traffic['external-to-thread'],
          'src-ipv6',
          'active-in'
        )
      }
    </div>
  `;

  if (isRouter) {
    const form = document.getElementById('device-form');

    if (form) {
      form.addEventListener('submit', event => {
        event.preventDefault();
        saveDeviceMetadata(raw.eui64, id);
      });
    }

    const editButton = document.getElementById('device-edit-button');

    if (editButton) {
      editButton.addEventListener('click', () => {
        renderDeviceMetadataForm(raw, id, metadata);
      });
    }

    const ipv6ToggleButton = document.getElementById('ipv6-toggle-button');

    if (ipv6ToggleButton) {
      ipv6ToggleButton.addEventListener('click', () => {
        const addresses = document.getElementById('ipv6-addresses');
        const isExpanded = addresses.hidden;

        addresses.hidden = !isExpanded;
        ipv6ToggleButton.setAttribute('aria-expanded', String(isExpanded));
        ipv6ToggleButton.textContent =
          isExpanded ? '▲ IPv6 アドレス' : '▼ IPv6 アドレス';
      });
    }
  }
}

function ipv6AddressesHtml(addresses) {
  if (!Array.isArray(addresses) || addresses.length === 0) {
    return '<p class="ipv6-addresses-empty">IPv6 アドレスは取得されていません。</p>';
  }

  return `
    <ul class="ipv6-addresses">
      ${addresses.map(address => `<li>${escapeHtml(address)}</li>`).join('')}
    </ul>
  `;
}

function renderDeviceMetadataForm(raw, nodeId, metadata) {
  const summary = document.getElementById('device-metadata-summary');

  summary.outerHTML = `
    <form class="device-form" id="device-form">
      <label>名前（必須）
        <input id="device-name" required value="${escapeHtml(metadata.name)}">
      </label>
      <label>場所
        <input id="device-location" value="${escapeHtml(metadata.location || '')}">
      </label>
      <label>種別
        <input id="device-type" value="${escapeHtml(metadata.type || '')}">
      </label>
      <button type="submit">保存</button>
      <p class="device-form-status" id="device-form-status"></p>
    </form>
  `;

  document.getElementById('device-form').addEventListener('submit', event => {
    event.preventDefault();
    saveDeviceMetadata(raw.eui64, nodeId);
  });
}

function shouldPreserveDeviceForm() {
  const form = document.getElementById('device-form');

  // Router の編集フォームは、ポーリングで置き換えない。保存時と
  // ノード選択時だけ renderDetail() が明示的に更新する。
  return form !== null;
}

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#039;');
}

function setDeviceMetadata(data) {
  deviceMetadataByEui64 = new Map(
    (data.devices || []).map(device => [
      device.eui64.toLowerCase(),
      device
    ])
  );
}

async function saveDeviceMetadata(eui64, nodeId) {
  const name = document.getElementById('device-name').value.trim();
  const location = document.getElementById('device-location').value.trim();
  const type = document.getElementById('device-type').value.trim();
  const status = document.getElementById('device-form-status');

  if (!name) {
    status.textContent = '名前を入力してください。';
    status.classList.add('error');
    return;
  }

  try {
    const response = await fetch(`${DEVICE_API_URL}/${eui64}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name, location, type })
    });

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const saved = await response.json();
    deviceMetadataByEui64.set(saved.eui64.toLowerCase(), saved);
    status.textContent = '保存しました。';
    status.classList.remove('error');

    if (lastNetworkInfo) {
      updateGraph(parseNetworkInfo(lastNetworkInfo));
      renderDetail(nodeId);
    }
  } catch (error) {
    console.error('save failed:', error);
    status.textContent = '保存に失敗しました。';
    status.classList.add('error');
  }
}


/* ============================================================
 * 接続状態
 * ============================================================ */

function setConnStatus(ok) {

  const dot =
    document.getElementById(
      'conn-dot'
    );

  const text =
    document.getElementById(
      'conn-text'
    );


  dot.classList.toggle(
    'offline',
    !ok
  );


  text.textContent =
    ok
      ? '接続中'
      : '接続エラー';


  document.getElementById(
    'last-updated'
  ).textContent =
    ok
      ? (
          '更新: ' +
          new Date()
            .toLocaleTimeString(
              'ja-JP'
            )
        )
      : '';
}


/* ============================================================
 * APIポーリング
 *
 * 2秒ごとにAPIへ問い合わせる。
 *
 * ただし、取得したデータをそのまま画面へ反映するのではなく、
 * updateGraph() 内で差分判定する。
 * ============================================================ */

async function poll() {

  try {

    const [res, devicesRes] = await Promise.all([
      fetch(API_URL, { cache: 'no-store' }),
      fetch(DEVICE_API_URL, { cache: 'no-store' })
    ]);


    if (!res.ok || !devicesRes.ok) {

      throw new Error(
        'HTTP ' + (!res.ok ? res.status : devicesRes.status)
      );

    }


    const data =
      await res.json();

    const devices =
      await devicesRes.json();

    setDeviceMetadata(devices);
    lastNetworkInfo = data;


    /*
     * ここで差分判定
     */

    const graph =
      parseNetworkInfo(
        data
      );


    updateGraph(
      graph
    );


    setConnStatus(
      true
    );

  } catch (err) {

    console.error(
      'poll failed:',
      err
    );


    setConnStatus(
      false
    );

  }
}


/* ============================================================
 * 起動
 * ============================================================ */

window.addEventListener(
  'load',
  () => {

    initSvg();

    /*
     * 初回取得
     */
    poll();

    /*
     * 2秒ごとに取得
     */
    setInterval(
      poll,
      POLL_INTERVAL_MS
    );

  }
);


/* ============================================================
 * ウィンドウサイズ変更
 * ============================================================ */

window.addEventListener(
  'resize',
  () => {
    resizeGraph();
  }
);
