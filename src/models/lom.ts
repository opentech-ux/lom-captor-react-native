import type { Rect } from '../types/geometry';

export interface RawRoot {
  tag: number;
  name: string;
  x?: number;
  y?: number;
  width?: number;
  height?: number;
  children?: RawRoot[];
}

export interface RootObject {
  id: number;
  parentId: number | null;
  depth: number;
  type: string;
  x: number;
  y: number;
  width: number;
  height: number;
  viewportX: number;
  viewportY: number;
}

export interface Root {
  id: number;
  tag: number;
  name: string;
  widgetType?: string;
  box: Rect;
  depth: number;
  children: Root[];
}
export interface Lom {
  /** Fabric routing metadata. Intentionally omitted from the public wire. */
  surfaceId: number;
  /** Structural identity of the normalized Root and every LomRef. */
  ref: string;
  id: string;
  timestamp: number;
  /** Context of this occurrence only; absent means unknown, [] means known root. */
  ar?: string[];
  root?: Root | null;
  width?: number;
  height?: number;
  fromCache: boolean;
}

export const toMapLom = (lom: Lom): Record<string, any> => {
  if (lom.fromCache) {
    return {
      ref: lom.ref,
      ts: lom.timestamp,
      ...(lom.ar !== undefined ? { ar: lom.ar } : {}),
    };
  }

  return {
    id: lom.id,
    ref: lom.ref,
    ts: lom.timestamp,
    ...(lom.ar !== undefined ? { ar: lom.ar } : {}),
    w: lom.width,
    h: lom.height,
    r: !lom.root ? '' : toMapRoot(lom.root),
  };
};
export const toMapRoot = (root: Root): Record<string, any> => {
  const box: number[] = [
    Math.trunc(root.box.left),
    Math.trunc(root.box.top),
    Math.trunc(root.box.width),
    Math.trunc(root.box.height),
  ];
  const widgetType = root.widgetType?.trim();

  const serialized: Record<string, any> = {
    id: `z${root.id}`,
    t: root.name,
    b: box,
    c: root.children.map(toMapRoot),
  };

  if (widgetType) {
    serialized.widgetType = widgetType;
  }

  return serialized;
};
