// automatically generated by the FlatBuffers compiler, do not modify

import * as flatbuffers from 'flatbuffers';

export class BSSID {
  bb: flatbuffers.ByteBuffer|null = null;
  bb_pos = 0;
  __init(i:number, bb:flatbuffers.ByteBuffer):BSSID {
  this.bb_pos = i;
  this.bb = bb;
  return this;
}

array(index: number):number|null {
    return this.bb!.readUint8(this.bb_pos + 0 + index);
}

static sizeOf():number {
  return 6;
}

static createBSSID(builder:flatbuffers.Builder, array: number[]|null):flatbuffers.Offset {
  builder.prep(1, 6);

  for (let i = 5; i >= 0; --i) {
    builder.writeInt8((array?.[i] ?? 0));

  }

  return builder.offset();
}

}