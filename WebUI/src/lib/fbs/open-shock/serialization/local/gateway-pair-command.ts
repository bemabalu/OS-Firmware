// automatically generated by the FlatBuffers compiler, do not modify

import * as flatbuffers from 'flatbuffers';

export class GatewayPairCommand {
  bb: flatbuffers.ByteBuffer|null = null;
  bb_pos = 0;
  __init(i:number, bb:flatbuffers.ByteBuffer):GatewayPairCommand {
  this.bb_pos = i;
  this.bb = bb;
  return this;
}

static getRootAsGatewayPairCommand(bb:flatbuffers.ByteBuffer, obj?:GatewayPairCommand):GatewayPairCommand {
  return (obj || new GatewayPairCommand()).__init(bb.readInt32(bb.position()) + bb.position(), bb);
}

static getSizePrefixedRootAsGatewayPairCommand(bb:flatbuffers.ByteBuffer, obj?:GatewayPairCommand):GatewayPairCommand {
  bb.setPosition(bb.position() + flatbuffers.SIZE_PREFIX_LENGTH);
  return (obj || new GatewayPairCommand()).__init(bb.readInt32(bb.position()) + bb.position(), bb);
}

code():string|null
code(optionalEncoding:flatbuffers.Encoding):string|Uint8Array|null
code(optionalEncoding?:any):string|Uint8Array|null {
  const offset = this.bb!.__offset(this.bb_pos, 4);
  return offset ? this.bb!.__string(this.bb_pos + offset, optionalEncoding) : null;
}

static startGatewayPairCommand(builder:flatbuffers.Builder) {
  builder.startObject(1);
}

static addCode(builder:flatbuffers.Builder, codeOffset:flatbuffers.Offset) {
  builder.addFieldOffset(0, codeOffset, 0);
}

static endGatewayPairCommand(builder:flatbuffers.Builder):flatbuffers.Offset {
  const offset = builder.endObject();
  return offset;
}

static createGatewayPairCommand(builder:flatbuffers.Builder, codeOffset:flatbuffers.Offset):flatbuffers.Offset {
  GatewayPairCommand.startGatewayPairCommand(builder);
  GatewayPairCommand.addCode(builder, codeOffset);
  return GatewayPairCommand.endGatewayPairCommand(builder);
}
}