class IntcodeInst<
	int opcode,
	dag outs,
	dag ins,
	string asmstr,
	list<dag> pattern
> : Instruction {
//	bits<!add(32, !mul(32, paramCount))> Inst;

	let Namespace = "LEG";
	dag OutOperandList = outs;
	dag InOperandList = ins;
}

class ArithBaseInst<int opcode, string opstr, SDNode opnode, Operand optype1, Operand optype2> : IntcodeInst<
	opcode,
	(outs memsrc:$dst),
	(ins optype1:$op1, optype2:$op2),
	!strconcat(opstr, " $dst, $op1, $op2"),
	[(set i64:$dst, (opnode i64:$op1, i64:$op2))]
> {
	bits<64> dst;
	bits<64> op1;
	bits<64> op2;
}

multiclass ArithInst<
	int opcode,
	string opstr,
	SDNode opnode,
> {
	def ii : ArithBaseInst<!add(opcode), opstr, opnode, i64imm, i64imm>;
	def im : ArithBaseInst<!add(1000 + opcode), opstr, opnode, i64imm, memsrc>;
	def mm : ArithBaseInst<!add(1100 + opcode), opstr, opnode, memsrc, memsrc>;
}

defm ADD : ArithInst<1, "add", add>;
defm MUL : ArithInst<2, "mul", mul>;

multiclass CmpInst<int opcode, string opstr, SDNode opnode, Operand optype1, Operand optype2> : IntcodeInst<
	opcode,
	(outs memsrc:$dst),
	(ins optype1:$op1, optype2:$op2),
	!strconcat(opstr, " $op1, $op2"),
	[(icmp )]