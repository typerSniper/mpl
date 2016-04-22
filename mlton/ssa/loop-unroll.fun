(* Copyright (C) 1999-2005, 2008 Henry Cejtin, Matthew Fluet, Suresh
 *    Jagannathan, and Stephen Weeks.
 * Copyright (C) 1997-2000 NEC Research Institute.
 *
 * MLton is released under a BSD-style license.
 * See the file MLton-LICENSE for details.
 *)

(* Reduces or eliminates the iteration count of loops by duplicating
 * the loop body.
 *)
functor LoopUnroll(S: SSA_TRANSFORM_STRUCTS): SSA_TRANSFORM = 
struct

open S
open Exp Transfer Prim

structure Graph = DirectedGraph
local
   open Graph
in
   structure Forest = LoopForest
end

val loopCount = ref 0
val optCount = ref 0
val multiHeaders = ref 0
val varEntryArg = ref 0
val variantTransfer = ref 0
val unsupported = ref 0
val ccTransfer = ref 0
val varBound = ref 0
val infinite = ref 0
val tooBig = ref 0
val floops = ref 0

fun ++ (v: int ref): unit =
  v := (!v) + 1

type BlockInfo = Label.t * (Var.t * Type.t) vector

structure Loop =
  struct
    datatype Bound = Eq of IntInf.t | Lt of IntInf.t | Gt of IntInf.t
    type Start = IntInf.t
    type Step = IntInf.t
    datatype t = T of {start: Start, step: Step, bound: Bound, invert: bool}

    fun toString (T {start, step, bound, invert}): string =
      let
        val boundStr = case bound of
          Eq b => if invert then
                    concat ["!= ", IntInf.toString b]
                  else
                    concat ["= ", IntInf.toString b]
        | Lt b => if invert then
                    concat ["!< ", IntInf.toString b]
                  else
                    concat ["< ", IntInf.toString b]
        | Gt b => if invert then
                    concat ["!> ", IntInf.toString b]
                  else
                    concat ["> ", IntInf.toString b]
      in
        concat[" Start: ", IntInf.toString start,
               " Step: ", IntInf.toString step,
               " Bound: ", boundStr]
      end
      
    fun isInfiniteLoop (T {start, step, bound, invert}): bool =
      case bound of
        Eq b =>
          if invert then
            (if start = b then
              false
            else if start < b andalso step > 0 then
              not (((b - start) mod step) = 0)
            else if start > b andalso step < 0 then
              not (((start - b) mod (~step)) = 0)
            else
              true)
          else
            step = 0
      | Lt b =>
        if invert then
          start >= b andalso step >= 0
        else
          start < b andalso step <= 0
      | Gt b =>
        if invert then
          start <= b andalso step <= 0
        else
          start > b andalso step >= 0


    fun iters (start: IntInf.t, step: IntInf.t, max: IntInf.t): IntInf.t =
      let
        val range = max - start
        val iters = range div step
        val adds = range mod step
      in
        if step > range then
          1
        else
          iters + adds
      end

    (* Assumes isInfiniteLoop is false, otherwise the result is undefined. *)
    fun iterCount (T {start, step, bound, invert}): IntInf.t =
      case bound of
        Eq b =>
          if invert then
            (b - start) div step
          else
            1
      | Lt b =>
        if (start >= b) andalso (not invert) then
          0
        else if invert then
          iters (b - 1, ~step, start)
        else
          iters (start, step, b)
      | Gt b =>
        if (start <= b) andalso (not invert) then
          0
        else if invert then
          iters (start, step, b + 1)
        else
          iters (b, ~step, start)

    fun makeStatement (v: IntInf.t, wsize: WordSize.t): Var.t * Statement.t =
      let
        val newWord = WordX.fromIntInf (v, wsize)
        val newConst = Const.word newWord
        val newExp = Exp.Const (newConst)
        val newType = Type.word wsize
        val newVar = Var.newNoname()
        val newStatement = Statement.T {exp = newExp,
                                        ty = newType,
                                        var = SOME(newVar)}
      in
        (newVar, newStatement)
      end

    (* Assumes isInfiniteLoop is false, otherwise this will run forever. *)
    fun makeConstants (T {start, step, bound, invert},
                     wsize: WordSize.t)
                     : Var.t list * Statement.t list =
      (* Even if the loop never runs, include a single iteration so that
         pre-transfer code won't be lost *)
      if (iterCount (T {start = start, step = step, bound = bound, invert = invert})) = 0 then
        let
          val (newVar, newStatement) = makeStatement(start, wsize)
        in
          ([newVar], [newStatement])
        end
      else
        case bound of
          Eq b =>
            if (start = b) <> invert then
              let
                val (newVar, newStatement) = makeStatement(start, wsize)
                val nextIter = T {start = start + step,
                                  step = step,
                                  bound = bound,
                                  invert = invert}
                val (rVars, rStmts) = makeConstants (nextIter, wsize)
              in
                (newVar::rVars, newStatement::rStmts)
              end
            else
              ([], [])
        | Lt b =>
            if (start < b) <> invert then
              let
                val (newVar, newStatement) = makeStatement(start, wsize)
                val nextIter = T {start = start + step,
                                  step = step,
                                  bound = bound,
                                  invert = invert}
                val (rVars, rStmts) = makeConstants (nextIter, wsize)
              in
                (newVar::rVars, newStatement::rStmts)
              end
            else
              ([], [])
        | Gt b =>
            if (start > b) <> invert then
              let
                val (newVar, newStatement) = makeStatement(start, wsize)
                val nextIter = T {start = start + step,
                                  step = step,
                                  bound = bound,
                                  invert = invert}
                val (rVars, rStmts) = makeConstants (nextIter, wsize)
              in
                (newVar::rVars, newStatement::rStmts)
              end
            else
              ([], [])
  end

fun logli (l: Layout.t, i: int): unit =
   Control.diagnostics
   (fn display =>
      display(Layout.indent(l, i * 2)))

fun logsi (s: string, i: int): unit =
   logli((Layout.str s), i)

fun logs (s: string): unit =
   logsi(s, 0)

fun logstat (x: int ref, s: string): unit =
  logs (concat[Int.toString(!x), " ", s])

fun listPop lst =
  case lst of
    [] => []
  | _::tl => tl

(* If a block was renamed, return the new name. Otherwise return the old name. *)
fun fixLabel (getBlockInfo: Label.t -> BlockInfo, 
              label: Label.t,
              origLabels: Label.t vector): Label.t =
  if Vector.contains(origLabels, label, Label.equals) then
    let
      val (name, _) = getBlockInfo(label)
    in
      name
    end
  else
    label

fun varOptEquals (v1: Var.t, v2: Var.t option): bool =
   case v2 of
     NONE => false
   | SOME (v2') => Var.equals (v1, v2')

(* For an binary operation where one argument is a constant,
   load that constant.
   Returns the variable, the constant, and true if the var was the first arg *)
fun varConst (args, loadVar) =
   let
      val a1 = Vector.sub (args, 0)
      val a2 = Vector.sub (args, 1)
      val a1v = loadVar(a1)
      val a2v = loadVar(a2)
   in
      case (a1v, a2v) of
        (SOME x, NONE) => SOME (a2, x, false)
      | (NONE, SOME x) => SOME (a1, x, true)
      | _ => NONE
   end

(* Given:
    - an argument vector with two arguments
    - a primative operaton that is an addition or subtraction of a const value
    - a function from variables to their constant values
   Returns:
    -  The non-const variable and the constant value in terms of addition *)
fun checkPrim (args, prim, loadVar) =
  case Prim.name prim of
    Name.Word_add _ =>
      (case varConst(args, loadVar) of
        SOME(nextVar, x, _) => SOME (nextVar, x)
      | NONE => NONE)
  | Name.Word_addCheck _ =>
      (case varConst(args, loadVar) of
        SOME(nextVar, x, _) => SOME(nextVar, x)
      | NONE => NONE)
  | Name.Word_sub _ =>
      (case varConst(args, loadVar) of
        SOME(nextVar, x, _) => SOME (nextVar, ~x)
      | NONE => NONE)
  | Name.Word_subCheck _ =>
      (case varConst(args, loadVar) of
        SOME(nextVar, x, _) => SOME (nextVar, ~x)
      | NONE => NONE)
  | _ => NONE

(* Given:
    - a variable in the loop
    - another variable in the loop
    - the loop body
    - a function from variables to their constant values
    - a starting value, if the transfer to the header is an arith transfer
   Returns:
    - Some x such that the value of origVar in loop iteration i+1 is equal to
      (the value of origVar in iteration i) + x,
      or None if the step couldn't be computed *)
fun varChain (origVar, endVar, blocks, loadVar, total) =
   case Var.equals (origVar, endVar) of
     true => SOME (total)
   | false =>
      let
        val endVarAssign = Vector.peekMap (blocks, fn b =>
          let
             val stmts = Block.statements b
             val assignments = Vector.keepAllMap (stmts, fn s =>
                case varOptEquals (endVar, Statement.var s) of
                  false => NONE
                | true =>
                   (case Statement.exp s of
                      Exp.PrimApp {args, prim, ...} => checkPrim (args, prim, loadVar)
                      | _ => NONE))
             val label = Block.label b
             val blockArgs = Block.args b
             (* If we found the assignment or the block isn't unary, skip this step *)
             val arithTransfers =
              if ((Vector.length assignments) > 0) orelse ((Vector.length blockArgs) <> 1)
              then
                Vector.new0 ()
              else
                let
                  val (blockArg, _) = Vector.sub (blockArgs, 0)
                  val blockEntrys = Vector.keepAllMap (blocks, fn b' =>
                    case Block.transfer b' of
                      Transfer.Arith {args, prim, success, ...} =>
                        if Label.equals (label, success) then
                           SOME(checkPrim(args, prim, loadVar))
                        else NONE
                    | Transfer.Call {return, ...} =>
                        (case return of
                           Return.NonTail {cont, ...} =>
                              if Label.equals (label, cont) then
                                 SOME(NONE)
                              else NONE
                         | _ => NONE)
                    | Transfer.Case {cases, ...} =>
                        (case cases of
                           Cases.Con v =>
                              if Vector.exists (v, fn (_, lbl) =>
                                 Label.equals (label, lbl)) then
                                   SOME(NONE)
                              else
                                 NONE
                         | Cases.Word (_, v) =>
                              if Vector.exists (v, fn (_, lbl) =>
                                 Label.equals (label, lbl)) then
                                   SOME(NONE)
                              else NONE)
                    | Transfer.Goto {args, dst} =>
                        if Label.equals (label, dst) then
                          SOME(SOME(Vector.sub (args, 0), 0))
                        else NONE
                    | _ => NONE)
                in
                  if Var.equals (endVar, blockArg) then
                    blockEntrys
                  else
                    Vector.new0 ()
                end  
             val assignments' =
              if Vector.length (arithTransfers) > 0 then
                case (Vector.fold (arithTransfers,
                                Vector.sub (arithTransfers, 0),
                                fn (trans, trans') =>
                                  case (trans, trans') of
                                    (SOME(a1, v1), SOME(a2, v2)) =>
                                      if Var.equals (a1, a2) andalso v1 = v2 then
                                        trans
                                      else
                                        NONE
                                  | _ => NONE)) of
                  SOME(a, v) => Vector.new1 (a, v)
                | NONE => assignments
              else
                assignments
          in
             case Vector.length assignments' of
                0 => NONE
              | 1 => SOME (Vector.sub (assignments', 0))
              | _ => raise Fail "Multiple assignments in SSA form!"
          end)
      in
         case endVarAssign of
           NONE => NONE
         | SOME (nextVar, x) =>
            varChain(origVar, nextVar, blocks, loadVar, x + total)
      end

(* Given:
    - a list of loop body labels
    - a transfer on a boolean value where one branch exits the loop and the other continues
   Returns:
    - the label that exits the loop
    - the label that continues the loop
    - true if the continue branch is the true branch
 *)
fun loopExit (loopLabels: Label.t vector, transfer: Transfer.t): (Label.t * Label.t * bool) =
  case transfer of
    (* This should be a case statement on a boolean,
       so all dsts should be unary.
       One should transfer outside the loop, the other inside. *)
    Transfer.Case {cases, default, ...} =>
      (case default of
        SOME(defaultLabel) =>
          let
            val (caseCon, caseLabel) =
              case cases of
                Cases.Con v => Vector.sub (v, 0)
              | _ => raise Fail "This should be a con"
          in
            if Vector.contains (loopLabels, defaultLabel, Label.equals) then
              (caseLabel, defaultLabel, Con.equals (Con.fromBool false, caseCon))
            else
              (defaultLabel, caseLabel, Con.equals (Con.fromBool true, caseCon))
          end    
      | NONE =>
          (case cases of
            Cases.Con v =>
              let
                val (c1, d1) = Vector.sub (v, 0)
                val (c2, d2) = Vector.sub (v, 1)
              in
                if Vector.contains (loopLabels, d1, Label.equals) then
                  (d2, d1, Con.equals (Con.fromBool true, c1))
                else
                  (d1, d2, Con.equals (Con.fromBool true, c2))
              end
            | _ => raise Fail "This should be a con"))
          
  | _ => raise Fail "This should be a case statement"

fun isLoopBranch (loopLabels, cases, default) =
  case default of
    SOME (defaultLabel) =>
      (case cases of
        Cases.Con v =>
          if (Vector.length v) = 1 then
            let
              val (_, caseLabel) = Vector.sub (v, 0)
              val defaultInLoop = Vector.contains (loopLabels, defaultLabel, Label.equals)
              val caseInLoop = Vector.contains (loopLabels, caseLabel, Label.equals)
              val () = logsi (concat["Comparing ",
                                     Label.toString defaultLabel,
                                     " and ",
                                     Label.toString caseLabel], 5)
              val () = logsi (Bool.toString (defaultInLoop <> caseInLoop), 5)
            in
              defaultInLoop <> caseInLoop 
            end
          else
            false
      | _ => false)
  | NONE =>
    (case cases of
      Cases.Con v =>
        if (Vector.length v) = 2 then
          let
            val (_, c1) = Vector.sub (v, 0)
            val (_, c2) = Vector.sub (v, 1)
            val () = logsi (concat["Comparing ",
                                   Label.toString c1,
                                   " and ",
                                   Label.toString c2], 5)
            val c1il = Vector.contains (loopLabels, c1, Label.equals)
            val c2il = Vector.contains (loopLabels, c2, Label.equals)
            val () = logsi (Bool.toString (c1il <> c2il), 5)
          in
            c1il <> c2il
          end
        else
          false
    | _ => false)


(* Given:
    - a loop phi variable
    - that variables index in the loop header's arguments
    - that variables constant entry value (if it has one)
    - the loop header block
    - the loop body block
    - a function from variables to their constant values
   Returns:
    - a Loop structure for unrolling that phi var, if one exists *)
fun checkArg ((argVar, _), argIndex, entryArg, header, loopBody, loadVar, depth) =
   case entryArg of
      NONE => (logsi ("Can't unroll: entry arg not constant", depth) ;
               ++varEntryArg ;
               NONE)
   | SOME (entryX) =>
      let
         val headerLabel = Block.label header
         val unsupportedTransfer = ref false

         (* For every transfer to the start of the loop, get the variable at argIndex *)
         val loopVars = Vector.keepAllMap (loopBody, fn block => 
            case Block.transfer block of
               Transfer.Arith {args, prim, success, ...} =>
                  if Label.equals (headerLabel, success) then
                     case checkPrim (args, prim, loadVar) of
                       NONE => (unsupportedTransfer := true ; NONE)
                     | SOME (arg, x) => SOME (arg, x)
                  else NONE
             | Transfer.Call {return, ...} =>
                  (case return of
                     Return.NonTail {cont, ...} =>
                        if Label.equals (headerLabel, cont) then
                           (unsupportedTransfer := true ; NONE)
                        else NONE
                   | _ => NONE)
             | Transfer.Case {cases, ...} =>
                 (case cases of
                    Cases.Con v =>
                     if Vector.exists(v, fn (_, lbl) =>
                                           Label.equals (headerLabel, lbl)) then
                        (unsupportedTransfer := true ; NONE)
                     else NONE
                  | Cases.Word (_, v) =>
                     if Vector.exists(v, fn (_, lbl) =>
                                           Label.equals (headerLabel, lbl)) then
                        (unsupportedTransfer := true ; NONE)
                     else NONE)
             | Transfer.Goto {args, dst} =>
                  if Label.equals (headerLabel, dst) then
                     SOME (Vector.sub (args, argIndex), 0)
                  else NONE
            | _ => NONE)
      in
         if (Vector.length loopVars) > 1
         andalso not (Vector.forall
                      (loopVars, fn (arg, x) =>
                        let
                          val (arg0, x0) = Vector.sub (loopVars, 0)
                        in
                          Var.equals (arg0, arg) andalso (x0 = x)
                        end))
         then
            (logsi ("Can't unroll: variant transfer to head of loop", depth) ;
             ++variantTransfer ;
             NONE)
         else if (!unsupportedTransfer) then
            (logsi ("Can't unroll: unsupported transfer to head of loop", depth) ;
             ++unsupported ;
             NONE)
         else
            let
               val (loopVar, x) = Vector.sub (loopVars, 0)
            in
               case varChain (argVar, loopVar, loopBody, loadVar, x) of
                 NONE => (logsi ("Can't unroll: can't compute transfer", depth) ; 
                          ++ccTransfer ;
                          NONE)
               | SOME (step) =>
                  let
                    fun ltOrGt (vc) =
                      case vc of
                        NONE => NONE
                      | SOME (_, c, b) =>
                          if b then
                            SOME(Loop.Lt (c))
                          else
                            SOME(Loop.Gt (c))

                    fun eq (vc) =
                      case vc of
                        NONE => NONE
                      | SOME (_, c, _) => SOME(Loop.Eq (c))
                    val loopLabels = Vector.map (loopBody, Block.label)
                    val transferVarBlock = Vector.peekMap (loopBody, (fn b =>
                      let
                        val transferVar =
                          case Block.transfer b of
                            Transfer.Case {cases, default, test} =>
                              if isLoopBranch (loopLabels, cases, default) then
                                SOME(test)
                              else NONE
                          | _ => NONE
                        val loopBound =
                          case (transferVar) of
                            NONE => NONE
                          | SOME (tVar) =>
                              Vector.peekMap (Block.statements b,
                                (fn s => case Statement.var s of
                                  NONE => NONE
                                | SOME (sVar) =>
                                  if Var.equals (tVar, sVar) then
                                    case Statement.exp s of
                                      PrimApp {args, prim, ...} =>
                                        if not (Vector.contains (args, argVar, Var.equals))
                                        then
                                           NONE
                                        else
                                          (case Prim.name prim of
                                            Name.Word_lt _ => ltOrGt (varConst (args, loadVar))
                                          | Name.Word_equal _ => eq (varConst (args, loadVar))
                                          | _ => NONE)
                                    | _ => NONE
                                  else NONE))
                      in
                        case loopBound of
                          NONE => NONE
                        | SOME (bound) =>
                            SOME(bound, b)
                      end))
                  in
                    case transferVarBlock of
                      NONE =>
                        (logsi ("Can't unroll: can't determine bound", depth) ;
                         ++varBound ;
                         NONE)
                    | SOME(bound, block) =>
                        let
                          val loopLabels = Vector.map (loopBody, Block.label)
                          val (_, _, contIsTrue) =
                                loopExit (loopLabels, Block.transfer block)
                        in
                          SOME (argIndex,
                                block,
                                Loop.T {start = entryX,
                                        step = step,
                                        bound = bound,
                                        invert = not contIsTrue})
                        end
                  end
            end
      end
(* Check all of a loop's entry point arguments to see if a constant value.
   Returns a list of int options where SOME(x) is always x for each entry. *)
fun findConstantStart (entryArgs: ((IntInf.t option) vector) vector):
                                                          (IntInf.t option) vector =
  if (Vector.length entryArgs) > 0 then                                                         
    Vector.rev (Vector.fold (entryArgs, Vector.sub (entryArgs, 0),
      fn (v1, v2) => Vector.fromList (Vector.fold2 (v1, v2, [], fn (a1, a2, lst) =>
        case (a1, a2) of
          (SOME(x1), SOME(x2)) =>
            if x1 = x2 then SOME(x1)::lst
            else NONE::lst
        | _ => NONE::lst))))
  else Vector.new0 ()

(* Look for any optimization opportunities in the loop. *)
fun findOpportunity(functionBody: Block.t vector,
                    loopBody: Block.t vector,
                    loopHeaders: Block.t vector,
                    loadGlobal: Var.t -> IntInf.t option,
                    depth: int):
                    (int * Block.t * Loop.t) option =
   if (Vector.length loopHeaders) = 1 then
      let
         val header = Vector.sub (loopHeaders, 0)
         val headerArgs = Block.args header
         val headerLabel = Block.label header
         val () = logsi (concat["Evaluating loop with header: ",
                                Label.toString headerLabel], depth)
         fun blockEquals (b1, b2) = Label.equals (Block.label b1, Block.label b2)
         val emptyArgs = SOME(Vector.new (Vector.length headerArgs, NONE))
         val entryArgs = Vector.keepAllMap(functionBody, fn block =>
                          if Vector.contains (loopBody, block, blockEquals) then NONE
                          else case Block.transfer block of
                             Transfer.Arith {success, ...} =>
                              if Label.equals (headerLabel, success) then
                                 emptyArgs
                              else NONE
                           | Transfer.Call {return, ...} =>
                              (case return of
                                 Return.NonTail {cont, ...} =>
                                    if Label.equals (headerLabel, cont) then
                                       emptyArgs
                                    else NONE
                               | _ => NONE)
                           | Transfer.Case {cases, ...} =>
                              (case cases of
                                 Cases.Con v =>
                                    if Vector.exists (v, fn (_, lbl) =>
                                       Label.equals (headerLabel, lbl)) then
                                         emptyArgs
                                    else
                                       NONE
                               | Cases.Word (_, v) =>
                                    if Vector.exists (v, fn (_, lbl) =>
                                       Label.equals (headerLabel, lbl)) then
                                         emptyArgs
                                    else NONE)
                          | Transfer.Goto {args, dst} =>
                              if Label.equals (dst, headerLabel) then
                                SOME(Vector.map (args, loadGlobal))
                              else NONE
                          | _ => NONE)
         val () = logsi (concat["Loop has ",
                               Int.toString (Vector.length entryArgs),
                               " entry points"], depth)
         val constantArgs = findConstantStart entryArgs
         val unrollableArgs =
          Vector.keepAllMapi
            (headerArgs, fn (i, arg) => (
               logsi (concat["Checking arg: ", Var.toString (#1 arg)], depth) ;
               checkArg (arg, i, Vector.sub (constantArgs, i),
                         header, loopBody, loadGlobal, depth + 1)))
      in
        if (Vector.length unrollableArgs) > 0 then
          SOME(Vector.sub (unrollableArgs, 0))
        else NONE
      end
   else
      (logsi ("Can't optimize: loop has more than 1 header", depth) ;
       multiHeaders := (!multiHeaders) + 1 ;
       NONE)

fun makeHeader(oldHeader, (newVars, newStmts), newEntry) =
  let
    val oldArgs = Block.args oldHeader
    val newArgs = Vector.map (oldArgs, fn (arg, _) => arg)
    val newTransfer = Transfer.Goto {args = newArgs, dst = newEntry}
  in
    (Block.T {args = oldArgs,
              label = Block.label oldHeader,
              statements = Vector.fromList newStmts,
              transfer = newTransfer},
     newVars)
  end

(* Copy an entire loop. In the header, rewrite the transfer to take the loop branch.
   In the transfers to the top of the loop, rewrite the transfer to goto next.
   Ensure that the header is the first element in the list.
   Replace all instances of argi with argVar *)
fun copyLoop(blocks: Block.t vector,
             nextLabel: Label.t,
             headerLabel: Label.t,
             tBlock: Block.t,
             argi: int,
             argVar: Var.t,
             blockInfo: Label.t -> BlockInfo,
             setBlockInfo: Label.t * BlockInfo -> unit): Block.t vector =
  let
    val labels = Vector.map (blocks, Block.label)
    (* Assign a new label for each block *)
    val newBlocks = Vector.map (blocks, fn b =>
        let
          val oldName = Block.label b
          val oldArgs = Block.args b
          val newName = Label.newNoname()
          val () = setBlockInfo(oldName, (newName, oldArgs))
        in
          Block.T {args = Block.args b,
                   label = newName,
                   statements = Block.statements b,
                   transfer = Block.transfer b}
        end)
    (* Rewrite the transfers of each block *)
    val fixedBlocks = Vector.map (newBlocks,
                                  fn Block.T {args, label, statements, transfer} =>
      let
        val f = fn l => fixLabel(blockInfo, l, labels)
        val isHeader = Label.equals (label, f(headerLabel))
        val (newArgs, unrolledArg) =
          if isHeader then
            (args, SOME(Vector.sub (args, argi)))
          else (args, NONE)
        val newStmts =
          if isHeader then
            case unrolledArg of
              NONE => statements
            | SOME(var, ty) =>
                let
                  val assignExp = Exp.Var (argVar)
                  val assign = Statement.T {exp = assignExp,
                                            ty = ty,
                                            var = SOME(var)}
                  val assignV = Vector.new1(assign)
                in
                  Vector.concat [assignV, statements]
                end
          else
            statements
        val newTransfer =
          if Label.equals (label, Block.label tBlock) then
            let
              val (_, contLabel, _) = loopExit(labels, transfer)
            in
              Transfer.Goto {args = Vector.new0 (), dst = f(contLabel)}
            end
          else
            case transfer of
              Transfer.Arith {args, overflow, prim, success, ty} =>
                if Label.equals (success, headerLabel) then
                  Transfer.Arith {args = args,
                                  overflow = f(overflow),
                                  prim = prim,
                                  success = nextLabel,
                                  ty = ty}
                else
                  Transfer.Arith {args = args,
                                  overflow = f(overflow),
                                  prim = prim,
                                  success = f(success),
                                  ty = ty}
            | Transfer.Call {args, func, return} =>
                let
                  val newReturn =
                    case return of
                      Return.NonTail {cont, handler} =>
                        let
                          val newHandler = case handler of
                                             Handler.Handle l => Handler.Handle(f(l))
                                           | _ => handler
                        in
                          Return.NonTail {cont = f(cont), handler = newHandler}
                        end
                    | _ => return
                in
                  Transfer.Call {args = args, func = func, return = newReturn}
                end
            | Transfer.Case {cases, default, test} =>
                let
                  val newCases = Cases.map(cases, f)
                  val newDefault = case default of
                                     NONE => default
                                   | SOME(l) => SOME(f(l))
                in
                  Transfer.Case {cases = newCases, default = newDefault, test = test}
                end
            | Transfer.Goto {args, dst} =>
                if Label.equals (dst, headerLabel) then
                  Transfer.Goto {args = args, dst = nextLabel}
                else
                  Transfer.Goto {args = args, dst = f(dst)}
            | Transfer.Runtime {args, prim, return} =>
                Transfer.Runtime {args = args, prim = prim, return = f(return)}
            | _ => transfer
      in
        Block.T {args = newArgs,
                 label = label,
                 statements = newStmts,
                 transfer = newTransfer}
      end)
  in
    Vector.rev fixedBlocks
  end

(* Unroll a loop. The header should ALWAYS be the first element in the returned list. *)
fun unrollLoop (oldHeader, tBlock, argi, loopBlocks, argLabels, blockInfo, setBlockInfo) =
  let
    val oldHeaderLabel = Block.label oldHeader
    val oldHeaderArgs = Block.args oldHeader
    val loopLabels = Vector.map (loopBlocks, Block.label)
  in
    case argLabels of
      [] =>
        let
          val (exitLabel, _, _) = loopExit (loopLabels, Block.transfer tBlock)
          val newTransfer = Transfer.Goto {args = Vector.new0 (),
                                           dst = exitLabel}
        in
          [Block.T {args = oldHeaderArgs,
                    label = Label.newNoname (),
                    statements = Vector.new0 (),
                    transfer = newTransfer}]
        end
    | hd::tl =>
        let
          val res = unrollLoop (oldHeader, tBlock, argi,
                                loopBlocks, tl, blockInfo, setBlockInfo)
          val nextBlockLabel = Block.label (List.first res)
          val newLoop = copyLoop(loopBlocks, nextBlockLabel, oldHeaderLabel, tBlock,
                                 argi, hd, blockInfo, setBlockInfo)
        in
          (Vector.toList newLoop) @ res
        end
  end

fun shouldOptimize (iterCount) =
  if iterCount > 10 then false
  else true

(* Attempt to optimize a single loop. Returns a list of blocks to add to the program
   and a list of blocks to remove from the program. *)
fun optimizeLoop(allBlocks, headerNodes, loopNodes,
                 nodeBlock, loadGlobal, depth) =
   let
      val () = ++loopCount
      val headers = Vector.map (headerNodes, nodeBlock)
      val loopBlocks = Vector.map (loopNodes, nodeBlock)
      val loopBlockNames = Vector.map (loopBlocks, Block.label)
      val optOpt =
            findOpportunity(allBlocks, loopBlocks, headers, loadGlobal, depth + 1)
      val {get = blockInfo: Label.t -> BlockInfo,
         set = setBlockInfo: Label.t * BlockInfo -> unit, destroy} =
            Property.destGetSet(Label.plist,
                                Property.initRaise("blockInfo", Label.layout))
   in
      case optOpt of
        NONE => ([], [])
      | SOME (argi, tBlock, loop) =>
          if Loop.isInfiniteLoop loop then
            (logsi ("Can't unroll: infinite loop", depth) ;
             ++infinite ;
             logsi (concat["Index: ", Int.toString argi, Loop.toString loop], depth) ;
             ([], []))
          else
            let
              val () = ++optCount
              val oldHeader = Vector.sub (headers, 0)
              val oldArgs = Block.args oldHeader
              val (oldArg, oldType) = Vector.sub (oldArgs, argi)
              val () = logsi (concat["Can unroll loop on ",
                                     Var.toString oldArg], depth)
              val () = logsi (concat["Index: ", Int.toString argi,
                                   Loop.toString loop], depth)
              val iterCount = Loop.iterCount loop
              val () = logsi (concat["Loop will run ",
                                     IntInf.toString iterCount,
                                     " times"], depth)
              val () = logsi (concat["Transfer block is ",
                                      Label.toString (Block.label tBlock)], depth)
              val go = shouldOptimize (iterCount)
            in
              if go (*andalso (!floops < 6)*) then
                let
                  val () = ++floops
                  val argSize = case Type.dest oldType of
                                  Type.Word wsize => wsize
                                | _ => raise Fail "Argument is not of type word"
                  val newEntry = Label.newNoname()
                  val (newHeader, argLabels) =
                    makeHeader (oldHeader, Loop.makeConstants (loop, argSize), newEntry)
                  (* For each induction variable value, copy the loop's body *)
                  val newBlocks = unrollLoop (oldHeader, tBlock, argi, loopBlocks, argLabels,
                                              blockInfo, setBlockInfo)
                  (* Fix the first entry's label *)
                  val firstBlock = List.first newBlocks
                  val args' = Block.args firstBlock
                  val statements' = Block.statements firstBlock
                  val transfer' = Block.transfer firstBlock
                  val newHead = Block.T {args = args',
                                         label = newEntry,
                                         statements = statements',
                                         transfer = transfer'}
                  val newBlocks' = newHeader::(newHead::(listPop newBlocks))
                  val () = destroy()
                  val () = logs "Adding blocks"   
                  val () = List.foreach (newBlocks', fn b => logli (Block.layout b, depth))
                in
                  (newBlocks', (Vector.toList loopBlockNames))
                end
              else
                (logsi ("Can't unroll: loop too big", depth) ;
                 ++tooBig ;
                ([], []))
            end
   end

(* Traverse sub-forests until the innermost loop is found. *)
fun traverseSubForest ({loops, notInLoop},
                       allBlocks,
                       enclosingHeaders,
                       labelNode, nodeBlock, loadGlobal) =
   if (Vector.length loops) = 0 then
      optimizeLoop(allBlocks, enclosingHeaders, notInLoop,
                   nodeBlock, loadGlobal, 1)
   else
      Vector.fold(loops, ([], []), fn (loop, (new, remove)) =>
         let
            val (nBlocks, rBlocks) =
               traverseLoop(loop, allBlocks, labelNode, nodeBlock, loadGlobal)
         in
            ((new @ nBlocks), (remove @ rBlocks))
         end)

(* Traverse loops in the loop forest. *)
and traverseLoop ({headers, child},
                  allBlocks,
                  labelNode, nodeBlock, loadGlobal) =
      traverseSubForest ((Forest.dest child), allBlocks,
                         headers, labelNode, nodeBlock, loadGlobal)

(* Traverse the top-level loop forest. *)
fun traverseForest ({loops, ...}, allBlocks, labelNode, nodeBlock, loadGlobal) =
  let
    (* Gather the blocks to add/remove *)
    val (newBlocks, blocksToRemove) =
      Vector.fold(loops, ([], []), fn (loop, (new, remove)) =>
        let
          val (nBlocks, rBlocks) =
            traverseLoop(loop, allBlocks, labelNode, nodeBlock, loadGlobal)
        in
          ((new @ nBlocks), (remove @ rBlocks))
        end)
    val keep: Block.t -> bool =
      (fn b => not (List.contains(blocksToRemove, (Block.label b), Label.equals)))
    val reducedBlocks = Vector.keepAll(allBlocks, keep)
  in
    (Vector.toList reducedBlocks) @ newBlocks
  end

(* Performs the optimization on the body of a single function. *)
fun optimizeFunction loadGlobal function =
   let
      val () = floops := 0
      val {graph, labelNode, nodeBlock} = Function.controlFlow function
      val {args, blocks, mayInline, name, raises, returns, start} = Function.dest function
      val () = logs (concat["Optimizing function: ", Func.toString name])
      val root = labelNode start
      val forest = Graph.loopForestSteensgaard(graph, {root = root})
      val newBlocks = traverseForest((Forest.dest forest),
                                     blocks, labelNode, nodeBlock, loadGlobal)
      val () = logstat (floops, "loops optimized")
   in
      Function.new {args = args,
                    blocks = Vector.fromList(newBlocks),
                    mayInline = mayInline,
                    name = name,
                    raises = raises,
                    returns = returns,
                    start = start}
   end

(* Entry point. *)
fun transform (Program.T {datatypes, globals, functions, main}) =
   let
      fun loadGlobal (var : Var.t): IntInf.t option =
         let
            fun matchGlobal v g =
               case Statement.var g of
                 NONE => false
               | SOME (v') => Var.equals (v, v')
         in
            case Vector.peek (globals, matchGlobal var) of
              NONE => NONE
            | SOME (stmt) =>
               (case Statement.exp stmt of
                  Exp.Const c =>
                     (case c of
                        Const.Word w => SOME(WordX.toIntInf w)
                      | _ => NONE)
                | _ => NONE)
         end
      val () = loopCount := 0
      val () = optCount := 0
      val () = multiHeaders := 0
      val () = varEntryArg := 0
      val () = variantTransfer := 0
      val () = unsupported := 0
      val () = ccTransfer := 0
      val () = varBound := 0
      val () = infinite := 0
      val () = tooBig := 0
      val () = logs "Unrolling loops\n"
      val optimizedFunctions = List.map (functions, optimizeFunction loadGlobal)
      val restore = restoreFunction {globals = globals}
      val () = logs "Performing SSA restore"
      val cleanedFunctions = List.map (optimizedFunctions, restore)
      val shrink = shrinkFunction {globals = globals}
      val () = logs "Performing shrink"
      val shrunkFunctions = List.map (cleanedFunctions, shrink)
      val () = logstat (loopCount,
                        "total innermost loops")
      val () = logstat (optCount,
                        "loops optimized")
      val () = logstat (multiHeaders,
                        "loops had multiple headers")
      val () = logstat (varEntryArg,
                        "variable entry values")
      val () = logstat (variantTransfer,
                        "loops had variant transfers to the header")
      val () = logstat (unsupported,
                        "loops had unsupported transfers to the header")
      val () = logstat (ccTransfer,
                        "loops had non-computable steps")
      val () = logstat (varBound,
                        "loops had variable bounds")
      val () = logstat (infinite,
                        "infinite loops")
      val () = logstat (tooBig,
                        "loops too large to unroll")
      val () = logs "Done."
   in
      Program.T {datatypes = datatypes,
                 globals = globals,
                 functions = shrunkFunctions,
                 main = main}
   end

end
