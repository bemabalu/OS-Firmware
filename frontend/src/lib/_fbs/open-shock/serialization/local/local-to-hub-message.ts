// automatically generated by the FlatBuffers compiler, do not modify

/* eslint-disable @typescript-eslint/no-unused-vars, @typescript-eslint/no-explicit-any, @typescript-eslint/no-non-null-assertion */

import * as flatbuffers from 'flatbuffers';

import { LocalToHubMessagePayload, unionToLocalToHubMessagePayload, unionListToLocalToHubMessagePayload } from '../../../open-shock/serialization/local/local-to-hub-message-payload';


export class LocalToHubMessage {
  bb: flatbuffers.ByteBuffer|null = null;
  bb_pos = 0;
  __init(i:number, bb:flatbuffers.ByteBuffer):LocalToHubMessage {
  this.bb_pos = i;
  this.bb = bb;
  return this;
}

static getRootAsLocalToHubMessage(bb:flatbuffers.ByteBuffer, obj?:LocalToHubMessage):LocalToHubMessage {
  return (obj || new LocalToHubMessage()).__init(bb.readInt32(bb.position()) + bb.position(), bb);
}

static getSizePrefixedRootAsLocalToHubMessage(bb:flatbuffers.ByteBuffer, obj?:LocalToHubMessage):LocalToHubMessage {
  bb.setPosition(bb.position() + flatbuffers.SIZE_PREFIX_LENGTH);
  return (obj || new LocalToHubMessage()).__init(bb.readInt32(bb.position()) + bb.position(), bb);
}

payloadType():LocalToHubMessagePayload {
  const offset = this.bb!.__offset(this.bb_pos, 4);
  return offset ? this.bb!.readUint8(this.bb_pos + offset) : LocalToHubMessagePayload.NONE;
}

payload<T extends flatbuffers.Table>(obj:any):any|null {
  const offset = this.bb!.__offset(this.bb_pos, 6);
  return offset ? this.bb!.__union(obj, this.bb_pos + offset) : null;
}

static startLocalToHubMessage(builder:flatbuffers.Builder) {
  builder.startObject(2);
}

static addPayloadType(builder:flatbuffers.Builder, payloadType:LocalToHubMessagePayload) {
  builder.addFieldInt8(0, payloadType, LocalToHubMessagePayload.NONE);
}

static addPayload(builder:flatbuffers.Builder, payloadOffset:flatbuffers.Offset) {
  builder.addFieldOffset(1, payloadOffset, 0);
}

static endLocalToHubMessage(builder:flatbuffers.Builder):flatbuffers.Offset {
  const offset = builder.endObject();
  return offset;
}

static finishLocalToHubMessageBuffer(builder:flatbuffers.Builder, offset:flatbuffers.Offset) {
  builder.finish(offset);
}

static finishSizePrefixedLocalToHubMessageBuffer(builder:flatbuffers.Builder, offset:flatbuffers.Offset) {
  builder.finish(offset, undefined, true);
}

static createLocalToHubMessage(builder:flatbuffers.Builder, payloadType:LocalToHubMessagePayload, payloadOffset:flatbuffers.Offset):flatbuffers.Offset {
  LocalToHubMessage.startLocalToHubMessage(builder);
  LocalToHubMessage.addPayloadType(builder, payloadType);
  LocalToHubMessage.addPayload(builder, payloadOffset);
  return LocalToHubMessage.endLocalToHubMessage(builder);
}
}