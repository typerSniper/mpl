functor LamI (structure F : sig
                            type 'a t
                            val futureLat : bool * (unit -> 'a) -> 'a t
                            val poll : 'a t -> 'a option
                            val touch : 'a t -> 'a
                        end)
        :> INTERACTIVE =
struct

datatype ('f, 'p, 'r) gftr = G of { gen : ('f -> 'r * ('p, 'p, 'r) gftr),
                                    name : string option,
                                    fut : ('r * ('p, 'p, 'r) gftr) F.t option
                                  }
type ('p, 'r) ftr = ('p, 'p, 'r) gftr
type ('p, 'r) view = 'r * ('p, 'r) ftr
type ('p, 'r) fftr = (unit, 'p, 'r) gftr
datatype ('p, 'r) aview = Now of ('p, 'r) view | Later of ('p, 'r) fftr

datatype ('f, 'p, 'r) egftr = EG of { egen : ('f -> 'r * ('p, 'p, 'r) egftr),
                                     dead : bool ref,
                                     ename : string,
                                     efut : ('r * ('p, 'p, 'r) egftr) F.t option
                                   }
type ('p, 'r) eftr = ('p, 'p, 'r) egftr
type ('p, 'r) eview = 'r * ('p, 'r) eftr

type ('p, 'r) efftr = (unit, 'p, 'r) egftr
datatype ('p, 'r) eaview = ENow of ('p, 'r) eview | ELater of ('p, 'r) efftr

exception Dead_ftr
exception Dead_ftr_name of string

fun ftr (gen : 'p -> ('p, 'r) view): ('p, 'r) ftr =
  G {gen = gen, name = NONE, fut = NONE}

(** Constructs an input stream from the given ticker.
*)
fun eftr (name: string) (gen: 'p -> ('p, 'r) eview) : ('p, 'r) eftr =
  EG {egen = gen, dead = ref false, ename = name, efut = NONE}

fun is_fut (G s) : bool =
    case #fut s of
        SOME _ => true
      | NONE => false

fun eis_fut (EG s) : bool =
    case #efut s of
        SOME _ => true
      | NONE => false

(** [query s t] queries the interactible s with prompt t.
Returns a response and continuation
@raise Dead_itr if s has already been used
@raise Dead_itr_name with the name of s if s has already been used
*)
fun query (G s : ('f, 'p, 'r) gftr) (p : 'f) : ('p, 'r) view =
    let val (v, G s') = (#gen s) p in
        (v, G {gen = (#gen s'), name = #name s, fut = NONE})
    end

fun aquery (G s : ('f, 'p, 'r) gftr) (p : 'f) : ('p, 'r) aview =
    case #fut s of
        NONE =>
        let val g = #gen s
            val f = F.futureLat (false, (fn () => g p))
        in
            case F.poll f of
                SOME (v, G s') =>
                Now (v, G {gen = #gen s', name = #name s, fut = NONE})
              | NONE => Later (G {gen = (fn () => F.touch f),
                                  name = #name s,
                                  fut = SOME f})
        end
        | SOME f =>
          (case F.poll f of
               SOME v => Now v
             | NONE => Later (G {gen = (fn () => F.touch f),
                                 name = #name s,
                                 fut = SOME f}))

(** [equery s t] queries the external interactible s with prompt t.
Returns a response and continuation.
@raise Dead_itr_name with the name of s if s has already been used
*)
fun equery (EG s : ('f, 'p, 'r) egftr) (p: 'f) : ('p, 'r) eview =
  if !(#dead s) then raise (Dead_ftr_name (#ename s))
  else
      ((#dead s) := true;
       let val (v, EG s') = (#egen s) p in
        (v, EG {egen = #egen s', dead = #dead s', ename = #ename s, efut = NONE})
       end
      )

fun eaquery lat (EG s : ('f, 'p, 'r) egftr) (p : 'f) : ('p, 'r) eaview =
    if !(#dead s) then raise (Dead_ftr_name (#ename s))
    else
    case #efut s of
        NONE =>
        let val g = #egen s
            val f = F.futureLat (lat, fn () => g p)
        in
            case F.poll f of
                SOME (v, EG s') => ENow (v, EG {egen = #egen s', dead = #dead s',
                                               ename = #ename s, efut = NONE})
              | NONE => ELater (EG {egen = (fn () => F.touch f),
                                    dead = ref false,
                                    ename = #ename s,
                                    efut = SOME f})
        end
        | SOME f =>
          (case F.poll f of
               SOME v => ENow v
             | NONE => ELater (EG {egen = (fn () => F.touch f),
                                   dead = ref false,
                                   ename = #ename s,
                                   efut = SOME f}))
(** Splits an external interactible. Returns two interactibles partitioning
the interactible.
If the interactible is named <i>name</i>, the returned interactibles
will be named <i>name1</i> and <i>name2</i>.
@raise Dead_itr_name with the name of s if s has already been used
*)
fun split (EG s: ('p, 'r) eftr) : ('p, 'r) eftr * ('p, 'r) eftr =
    if !(#dead s) then raise (Dead_ftr_name (#ename s))
    else
        ((#dead s) := true;
         (EG {egen = #egen s, dead = ref false, ename = (#ename s) ^ "1",
              efut = NONE},
          EG {egen = #egen s, dead = ref false, ename = (#ename s) ^ "2",
              efut = NONE}))

end

structure LamIFut = LamI (structure F = MLton.Parallel.FutureSuspend)
