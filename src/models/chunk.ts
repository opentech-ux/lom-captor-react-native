import {
  concatenateAction,
  concatenateExploration,
  type ActionEvent,
  type ExplorationEvent,
} from './gestures';
import { toMapLom, type Lom } from './lom';
import type { SessionEnvironment } from './environment';

const LIB_V = '1.0.0';
const LIB_T = 'mobile';

export interface Chunk {
  sId: string;
  timestamp: number;
  env: SessionEnvironment;
  loms: Lom[];
  explorationEvents: ExplorationEvent[];
  actionEvents: ActionEvent[];
}

export const createChunk = (
  sessionId: string,
  environment: SessionEnvironment
): Chunk => ({
  sId: sessionId,
  timestamp: Date.now(),
  env: environment,
  loms: [],
  explorationEvents: [],
  actionEvents: [],
});

export const toMapChunk = (chunk: Chunk): Record<string, any> => {
  return {
    lib_v: LIB_V,
    env: chunk.env,
    type: LIB_T,
    ts: chunk.timestamp,
    sid: chunk.sId,
    loms: chunk.loms.map(toMapLom),
    pnt: [],
    ee: chunk.explorationEvents.map(concatenateExploration),
    ae: chunk.actionEvents.map(concatenateAction),
  };
};
export const isChunkEmpty = (chunk: Chunk): boolean => {
  return (
    chunk.loms.length === 0 &&
    chunk.actionEvents.length === 0 &&
    chunk.explorationEvents.length === 0
  );
};
