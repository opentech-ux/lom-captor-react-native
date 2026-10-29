import type { ResolvedSessionRecorderConfig } from '../models/config';
import type { Lom, Root, RootObject } from '../models/lom';
import { createUuidV7 } from '../utils/Uuid';
import { structuralLomRef } from './LomStructuralRef';

export interface LomTreeConfig {
  pruneAt: string[];
  noiseAt: string[];
}

export interface NonInformativeTree {
  readonly nonInformative: true;
}

export class LomTreeInspector {
  private static readonly MAX_CACHED_REFS = 4096;
  private _config: { pruneAt: string[]; noiseAt: string[] };
  private _sessionConfig: ResolvedSessionRecorderConfig;
  private _counter = 0;
  private _cache = new Map<string, Lom>();

  constructor(config: ResolvedSessionRecorderConfig) {
    this._config = {
      pruneAt: ['DebuggingOverlay'],
      noiseAt: ['RootNode', 'Fragment'],
    };
    this._sessionConfig = config;
  }

  public createLom(
    surfaceId: number,
    payload: any,
    viewport?: { viewportWidth: number; viewportHeight: number },
    ar?: string[]
  ): Lom | NonInformativeTree | null {
    const roots: any[] = payload.roots;

    if (!roots || roots.length === 0) return null;
    const viewportWidth = viewport?.viewportWidth;
    const viewportHeight = viewport?.viewportHeight;
    if (
      typeof viewportWidth !== 'number' ||
      typeof viewportHeight !== 'number' ||
      !Number.isFinite(viewportWidth) ||
      !Number.isFinite(viewportHeight) ||
      viewportWidth <= 0 ||
      viewportHeight <= 0
    ) {
      return null;
    }

    this._counter = 0;
    const builtRoot = this._buildRoot(roots, viewportWidth, viewportHeight);
    if (!builtRoot) return null;

    this._counter = 0;
    const root = this._collapseRedundantViews(builtRoot);

    const candidate =
      root.tag === -1 && root.name === 'Viewport' && root.children.length === 1
        ? root.children[0]!
        : root;
    if (
      candidate.name === 'RNSScreen' &&
      candidate.children.length === 0 &&
      Math.trunc(candidate.box.left) === 0 &&
      Math.trunc(candidate.box.top) === 0 &&
      Math.trunc(candidate.box.width) === Math.trunc(viewportWidth) &&
      Math.trunc(candidate.box.height) === Math.trunc(viewportHeight)
    ) {
      return { nonInformative: true };
    }

    // Native signature describes the flat pre-normalization vector only.
    // Structural identity is derived from the final normalized Root.
    const ref = structuralLomRef(root);
    const cacheKey = `${surfaceId}:${ref}`;
    const cached = this._cache.get(cacheKey);
    if (cached) {
      this._cache.delete(cacheKey);
      this._cache.set(cacheKey, cached);
      return {
        surfaceId,
        id: cached.id,
        ref,
        timestamp: Date.now(),
        ...(ar !== undefined ? { ar } : {}),
        root: cached.root,
        fromCache: true,
      };
    }

    const lom: Lom = {
      surfaceId,
      id: createUuidV7(),
      ref,
      timestamp: Date.now(),
      ...(ar !== undefined ? { ar } : {}),
      width: Math.trunc(viewportWidth),
      height: Math.trunc(viewportHeight),
      root: root,
      fromCache: false,
    };

    this._cache.set(cacheKey, lom);
    if (this._cache.size > LomTreeInspector.MAX_CACHED_REFS) {
      const oldestKey = this._cache.keys().next().value;
      if (oldestKey !== undefined) this._cache.delete(oldestKey);
    }

    return lom;
  }

  public discardUnrecordedLom(lom: Lom): void {
    if (lom.fromCache) return;
    const cacheKey = `${lom.surfaceId}:${lom.ref}`;
    if (this._cache.get(cacheKey)?.id === lom.id) this._cache.delete(cacheKey);
  }

  private _collapseRedundantViews(root: Root, collapsedDepth = 0): Root {
    let normalizedRoot = root;
    while (
      normalizedRoot.name === 'View' &&
      normalizedRoot.children.length === 1
    ) {
      const child = normalizedRoot.children[0]!;
      if (
        child.name !== 'View' ||
        Math.trunc(normalizedRoot.box.left) !== Math.trunc(child.box.left) ||
        Math.trunc(normalizedRoot.box.top) !== Math.trunc(child.box.top) ||
        Math.trunc(normalizedRoot.box.width) !== Math.trunc(child.box.width) ||
        Math.trunc(normalizedRoot.box.height) !== Math.trunc(child.box.height)
      ) {
        break;
      }
      normalizedRoot = child;
      collapsedDepth += 1;
    }

    // Keep logical IDs in pre-order and shift depth only for removed levels.
    normalizedRoot.id = ++this._counter;
    normalizedRoot.depth -= collapsedDepth;
    normalizedRoot.children = normalizedRoot.children.map((child) =>
      this._collapseRedundantViews(child, collapsedDepth)
    );
    return normalizedRoot;
  }

  private _buildRoot(
    roots: RootObject[],
    viewportWidth: number,
    viewportHeight: number
  ): Root | null {
    const rootMap = new Map<number, RootObject>();
    const childrenRootMap = new Map<number, RootObject[]>();

    for (const root of roots) {
      rootMap.set(root.id, root);

      const parentId = root.parentId ?? -1;
      if (!childrenRootMap.has(parentId)) {
        childrenRootMap.set(parentId, []);
      }

      childrenRootMap.get(parentId)?.push(root);
    }

    const topLevelRoots = roots.filter(
      (r) => r.parentId === null || !rootMap.has(r.parentId)
    );
    if (topLevelRoots.length === 0) return null;

    // A single Root is required on the wire. When Fabric supplies a forest,
    // or the top-level node is flattened, reserve a neutral logical container
    // before visiting children so IDs remain pre-order without inventing a widget.
    const firstTopLevelRoot = topLevelRoots[0]!;
    const firstTopLevelIsFlattened =
      this._config.noiseAt.includes(firstTopLevelRoot.type) ||
      firstTopLevelRoot.width === 0 ||
      firstTopLevelRoot.height === 0;
    const needsSyntheticRoot =
      topLevelRoots.length > 1 || firstTopLevelIsFlattened;
    const syntheticId = needsSyntheticRoot ? ++this._counter : null;
    const normalizedRoots: Root[] = [];
    for (const topLevelRoot of topLevelRoots) {
      normalizedRoots.push(
        ...this._visitChildren(
          topLevelRoot.id,
          rootMap,
          childrenRootMap,
          syntheticId === null ? 0 : 1
        )
      );
    }
    if (normalizedRoots.length === 0) return null;
    if (syntheticId === null) return normalizedRoots[0] ?? null;

    return {
      id: syntheticId,
      tag: -1,
      name: 'Viewport',
      box: { left: 0, top: 0, width: viewportWidth, height: viewportHeight },
      depth: 0,
      children: normalizedRoots,
    };
  }

  private _visitChildren(
    rootId: number,
    rootMap: Map<number, RootObject>,
    childrenMap: Map<number, RootObject[]>,
    depthOffset = 0
  ): Root[] {
    const r = rootMap.get(rootId);
    if (!r) return [];

    const originalChildren = childrenMap.get(rootId) || [];
    const wrapsDebuggingOverlay =
      originalChildren.length === 1 &&
      originalChildren[0]?.type === 'DebuggingOverlay';
    if (wrapsDebuggingOverlay) {
      return [];
    }

    if (this._config.pruneAt.includes(r.type)) {
      return [];
    }

    const hasNotSize = r.width === 0 || r.height === 0;
    if (this._config.noiseAt.includes(r.type) || hasNotSize) {
      const children: Root[] = [];
      for (const child of childrenMap.get(rootId) || []) {
        children.push(
          ...this._visitChildren(child.id, rootMap, childrenMap, depthOffset)
        );
      }
      return children;
    }

    const id = ++this._counter;
    const children: Root[] = [];
    for (const child of childrenMap.get(rootId) || []) {
      children.push(
        ...this._visitChildren(child.id, rootMap, childrenMap, depthOffset)
      );
    }

    const componentType = typeof r.type === 'string' ? r.type.trim() : '';
    const widgetType =
      typeof __DEV__ !== 'undefined' &&
      __DEV__ === true &&
      this._sessionConfig.debugShowTree &&
      componentType.length > 0 &&
      componentType !== 'Unknown'
        ? componentType
        : undefined;

    const root: Root = {
      id,
      tag: r.id,
      name: r.type,
      ...(widgetType ? { widgetType } : {}),
      box: {
        left: r.x || 0,
        top: r.y || 0,
        width: r.width || 0,
        height: r.height || 0,
      },
      depth: r.depth + depthOffset,
      children: children,
    };

    return [root];
  }
}
