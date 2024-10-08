// automatically generated by the FlatBuffers compiler, do not modify

/* eslint-disable @typescript-eslint/no-unused-vars, @typescript-eslint/no-explicit-any, @typescript-eslint/no-non-null-assertion */

import * as flatbuffers from 'flatbuffers';

import { SetGPIOResultCode } from '../../../open-shock/serialization/local/set-gpioresult-code';


export class SetEstopPinCommandResult {
  bb: flatbuffers.ByteBuffer|null = null;
  bb_pos = 0;
  __init(i:number, bb:flatbuffers.ByteBuffer):SetEstopPinCommandResult {
  this.bb_pos = i;
  this.bb = bb;
  return this;
}

static getRootAsSetEstopPinCommandResult(bb:flatbuffers.ByteBuffer, obj?:SetEstopPinCommandResult):SetEstopPinCommandResult {
  return (obj || new SetEstopPinCommandResult()).__init(bb.readInt32(bb.position()) + bb.position(), bb);
}

static getSizePrefixedRootAsSetEstopPinCommandResult(bb:flatbuffers.ByteBuffer, obj?:SetEstopPinCommandResult):SetEstopPinCommandResult {
  bb.setPosition(bb.position() + flatbuffers.SIZE_PREFIX_LENGTH);
  return (obj || new SetEstopPinCommandResult()).__init(bb.readInt32(bb.position()) + bb.position(), bb);
}

gpioPin():number {
  const offset = this.bb!.__offset(this.bb_pos, 4);
  return offset ? this.bb!.readInt8(this.bb_pos + offset) : 0;
}

result():SetGPIOResultCode {
  const offset = this.bb!.__offset(this.bb_pos, 6);
  return offset ? this.bb!.readUint8(this.bb_pos + offset) : SetGPIOResultCode.Success;
}

static startSetEstopPinCommandResult(builder:flatbuffers.Builder) {
  builder.startObject(2);
}

static addGpioPin(builder:flatbuffers.Builder, gpioPin:number) {
  builder.addFieldInt8(0, gpioPin, 0);
}

static addResult(builder:flatbuffers.Builder, result:SetGPIOResultCode) {
  builder.addFieldInt8(1, result, SetGPIOResultCode.Success);
}

static endSetEstopPinCommandResult(builder:flatbuffers.Builder):flatbuffers.Offset {
  const offset = builder.endObject();
  return offset;
}

static createSetEstopPinCommandResult(builder:flatbuffers.Builder, gpioPin:number, result:SetGPIOResultCode):flatbuffers.Offset {
  SetEstopPinCommandResult.startSetEstopPinCommandResult(builder);
  SetEstopPinCommandResult.addGpioPin(builder, gpioPin);
  SetEstopPinCommandResult.addResult(builder, result);
  return SetEstopPinCommandResult.endSetEstopPinCommandResult(builder);
}
}