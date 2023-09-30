open Core
open Ir

let tmp_counter = ref 0
let gen_tmp_name () =
  let counter_value = !tmp_counter in
  tmp_counter := counter_value + 1;
  let func prefix = "tmp" ^ prefix ^ (Int.to_string counter_value) in
  func

let render_conditions assumptions ~is_assert =
  let rec flatten_assumption (ass: tterm) = match ass.v with
    (* yeah that bool vs int 0 stuff is annoying, but writing a method to handle
       it would be more annoying... for now *)
    | Bop (Eq,
           {v=Int 0;t=Boolean},
           {v=Bop (Eq, {v=Int 0;t=Boolean}, tt);t=_}) -> flatten_assumption tt
    | Bop (Eq,
           {v=Int 0;t=Boolean},
           {v=Bop (Eq, {v=Bool false;t=_}, tt);t=_}) -> flatten_assumption tt
    | Bop (Eq,
           {v=Bool false;t=_},
           {v=Bop (Eq, {v=Int 0;t=Boolean}, tt);t=_}) -> flatten_assumption tt
    | Bop (Eq,
           {v=Bool false;t=_},
           {v=Bop (Eq, {v=Bool false;t=_}, tt);t=_}) -> flatten_assumption tt
    | Bop (Eq, {v=Int 0;t=Boolean}, {v=Bop (Or, x, y);t=_}) ->
      (flatten_assumption {v=Bop (Eq, {v=Bool false;t=Boolean}, x);t=Boolean}) @
      (flatten_assumption {v=Bop (Eq, {v=Bool false;t=Boolean}, y);t=Boolean})
    | Bop (Eq, {v=Bool false;t=_}, {v=Bop (Or, x, y);t=_}) ->
      (flatten_assumption {v=Bop (Eq, {v=Bool false;t=Boolean}, x);t=Boolean}) @
      (flatten_assumption {v=Bop (Eq, {v=Bool false;t=Boolean}, y);t=Boolean})
    | Bop (Eq, {v=Int 0;t=Boolean}, {v=Bop (And, x, y);t=_}) ->
      flatten_assumption {v=Bop (Or,
                                 {v=Bop (Eq, {v=Bool false;t=Boolean}, x);
                                  t=Boolean},
                                 {v=Bop (Eq, {v=Bool false;t=Boolean},y);
                                  t=Boolean});
                          t=Boolean}
    | Bop (Eq, {v=Bool false;t=_}, {v=Bop (And, x, y);t=_}) ->
      flatten_assumption {v=Bop (Or,
                                 {v=Bop (Eq, {v=Bool false;t=Boolean}, x);
                                  t=Boolean},
                                 {v=Bop (Eq, {v=Bool false;t=Boolean},y);
                                  t=Boolean});
                          t=Boolean}
    | Bop (And, x, y) -> (flatten_assumption x) @ (flatten_assumption y)
    | _ -> [ass]
  in
  let flattened_assumptions =
    List.concat (List.map assumptions ~f:flatten_assumption)
  in
  let fn = if is_assert then "//@ assert" else "//@ assume" in
  let rendered_assumptions = List.map flattened_assumptions ~f:(fun t ->
      match t.v with
      (* order-independent would be better you say? sure! but that's too bit a
         refactoring... #ResearchCode *)
      (* also, yes, this will result in the rather comical but efficient 'false
         is in {A,B}' for (false == A || false == B) *)
      | Bop (Or, {v=Bop (Eq, x1, a);t=_}, {v=Bop (Eq, x2, b);t=_})
        when x1 = x2 && a.t = b.t ->
        fn ^ "(mem(" ^ (render_tterm x1) ^ ", cons(" ^ (render_tterm a) ^
        ", cons(" ^ (render_tterm b) ^ ", nil))));"
      | Bop (Or, {v=Bop (Eq, a, x1);t=_}, {v=Bop (Eq, x2, b);t=_})
        when x1 = x2 && a.t = b.t ->
        fn ^ "(mem(" ^ (render_tterm x1) ^ ", cons(" ^ (render_tterm a) ^
        ", cons(" ^ (render_tterm b) ^ ", nil))));"
      | Bop (Or, {v=Bop (Eq, x1, a);t=_}, {v=Bop (Eq, b, x2);t=_})
        when x1 = x2 && a.t = b.t ->
        fn ^ "(mem(" ^ (render_tterm x1) ^ ", cons(" ^ (render_tterm a) ^
        ", cons(" ^ (render_tterm b) ^ ", nil))));"
      | Bop (Or, {v=Bop (Eq, a, x1);t=_}, {v=Bop (Eq, b, x2);t=_})
        when x1 = x2 && a.t = b.t ->
        fn ^ "(mem(" ^ (render_tterm x1) ^ ", cons(" ^ (render_tterm a) ^
        ", cons(" ^ (render_tterm b) ^ ", nil))));"
      (* so we might as well apply it for normal Ors as well! less forking *)
      | Bop (Or, a, b) -> fn ^"(mem(true, cons(" ^ (render_tterm a) ^
                          ", cons(" ^ (render_tterm b) ^ ", nil))));"
      | Id x -> fn ^ "(" ^ x ^ " != 0);"
      | Bop (Bit_and, _, _) -> fn ^ "(0 != " ^ (render_tterm t) ^ ");"
      | _ -> fn ^ "(" ^ (render_tterm t) ^ ");") in
  let unique_assumptions =
    String.Set.to_list (String.Set.of_list rendered_assumptions)
  in
  (String.concat ~sep:"\n" unique_assumptions) ^ "\n"


let rec render_eq_sttmt ~is_assert out_arg (out_val:tterm) =
  let head = (if is_assert then "assert" else "assume") in
  match out_val.v, out_val.t with
  (* HACKY HACK - can't do an assume over the arrays themselves because they're
     pointers and VeriFast will assume that the pointers, not the contents, are
     equal *)
  | Array cells, Array Uint8 ->
    begin match out_arg.t with
      | Array Uint8 ->
        let tmp_gen = gen_tmp_name() in
        let (bindings, expr) =
          Fspec_api.generate_2step_dereference out_arg tmp_gen
        in
        (String.concat ~sep:"\n" bindings) ^ "\n" ^
        (String.concat ~sep:"\n"
           (List.mapi cells ~f:(fun idx cell ->
                "//@ " ^ head ^ "(" ^ (render_tterm expr) ^
                "[" ^ (string_of_int idx) ^ "] == " ^ (render_tterm cell) ^ ");"
              ))) ^ "\n"
      | _ -> failwith "Arrays must be of type Uint8 (sorry!)" end
  (* A struct and its first member have the same address... oh and this is a
     hack so let's support doubly-nested structs *)
  | Id ovid, Uint16 ->
    begin match out_arg.v, out_arg.t with
      | Id oaid, Str (_, (outerfname,Str(_,(fname,_)::_))::_) ->
        "//@ " ^ head ^ "(" ^ oaid ^ "." ^ outerfname ^ "." ^
        fname ^ " == " ^ ovid ^ ");\n"
      | Id oaid, Str (_, (fname,_)::_) ->
        "//@ " ^ head ^ "(" ^ oaid ^ "." ^ fname ^ " == " ^ ovid ^ ");\n"
      | _, _ -> "//@ " ^ head ^ "(" ^ (render_tterm out_arg) ^ " == " ^
                (render_tterm out_val) ^ ");\n"
    end
  | Id _, Ptr _ -> (* HUGE HACK assume the type is wrongly guessed and it's
                      actually an integer *)
      render_eq_sttmt ~is_assert:is_assert out_arg {v=out_val.v;t=Uint16}
  (* Don't use == over structs, VeriFast doesn't understand it and returns a
     confusing message about dereferencing pointers *)
  | Id ovid, Str (_, ovfields) ->
    begin match out_arg.v, out_arg.t with
      | Id _, Ptr _ -> (* HUGE HACK assume the type is wrongly guessed and it's
                          actually an integer *)
        render_eq_sttmt ~is_assert:is_assert out_val {v=out_arg.v;t=Uint16}
      | Id _, Uint16 ->
        render_eq_sttmt ~is_assert:is_assert out_val out_arg
      | Id oaid, _ ->
        if out_val.t <> out_arg.t then
          failwith ("not the right type! " ^ ovid ^ ":" ^
                    (ttype_to_str out_val.t) ^ " <> " ^ oaid ^ ":" ^
                    (ttype_to_str out_arg.t));
        String.concat (List.map ovfields ~f:(fun (name,_) ->
            "//@ " ^ head ^ "(" ^ ovid ^ "." ^ name ^ " == " ^
            oaid ^ "." ^ name ^ ");\n"))

      | _ -> failwith ("not supported, sorry: " ^ (render_tterm out_arg) ^
                       ": " ^ (ttype_to_str out_arg.t) ^ " == " ^ ovid ^
                       " :" ^ (ttype_to_str out_val.t))
    end
  | Addr ptee, _ ->
    begin match out_arg.t with
      | Ptr Void ->
        render_eq_sttmt
          ~is_assert
          {v=Deref {v=Cast (out_val.t, out_arg);t=out_val.t};
           t=get_pointee out_val.t}
          ptee
      | _ ->
        render_eq_sttmt
          ~is_assert
          {v=Deref out_arg;t=get_pointee out_arg.t}
          ptee
    end
  | Struct (_, fields), _ ->
    if out_val.t <> out_arg.t then
      failwith ("arg and val types inconsistent: arg:" ^
                (ttype_to_str out_arg.t) ^ " <> val: " ^
                (ttype_to_str out_val.t));
    (match out_arg with
     | {v=Deref {v=Id arg_name;t=_};t=_} ->
       "//@ " ^ head ^ "(" ^ arg_name ^ "!= 0 );\n"
     (* can be important to know the arg can be read *)
     | _ -> "") ^
    String.concat (List.map fields ~f:(fun {name;value} ->
        render_eq_sttmt ~is_assert {v=Str_idx (out_arg, name);t=value.t} value))
  | Undef, _ ->
    failwith ("render_eq_sttmt undef for " ^ (render_tterm out_arg) ^
              " with val " ^ (render_tterm out_val))
  | _, _ -> "//@ " ^ head ^ "(" ^ (render_tterm out_arg) ^ " == " ^
            (render_tterm out_val) ^ ");\n"

let render_fcall_with_prelemmas context =
  (String.concat ~sep:"\n" context.pre_lemmas) ^ "\n" ^
  (match context.ret_name with
   | Some name -> (ttype_to_str context.ret_type) ^
                  " " ^ name ^ " = "
   | None -> "") ^
  (render_term context.application) ^ ";\n"

let render_postlemmas context =
  (String.concat ~sep:"\n" context.post_lemmas) ^ "\n"

let render_args_post_conditions ~is_assert apk =
  (String.concat ~sep:"\n" (List.map apk
                              ~f:(fun {lhs;rhs;} ->
                                  render_eq_sttmt ~is_assert
                                    lhs rhs))) ^ "\n"

let render_post_assumptions post_statements =
  render_conditions post_statements ~is_assert:false

let render_ret_equ_sttmt ~is_assert ret_name ret_type ret_val =
  match ret_name with
  | Some name ->
    (render_eq_sttmt ~is_assert {v=Id name;t=ret_type} ret_val) ^ "\n"
  | None -> "\n"

let render_assignment {lhs;rhs;} =
  match rhs.v with
  | Undef -> "";
  | _ -> begin match rhs.t,rhs.v with
      | Array Uint8, Array cells ->
        String.concat ~sep:"\n" (List.mapi cells ~f:(fun idx cell ->
            (render_tterm lhs) ^ "[" ^ (string_of_int idx) ^ "] = " ^
            (render_tterm cell) ^ ";"))
      | Array _, _ -> failwith ((render_tterm lhs) ^ " = " ^
                                (render_tterm rhs) ^
                               " is not handled")
      | _ -> (render_tterm lhs) ^ " = " ^ (render_tterm rhs) ^ ";"
    end

let rec gen_plain_equalities {lhs;rhs} =
  if term_eq lhs.v rhs.v then []
  else match rhs.t, rhs.v with
  | _, Undef -> []
  | Array _, Array cells -> begin match lhs.t with
      | Array _ -> [{lhs;rhs}]
      | Ptr ptee_t ->
        List.mapi cells ~f:(fun idx value ->
            {lhs={v=Deref {v=Bop (Add, lhs, {v=Int idx;t=Uint32});t=lhs.t};
                  t=ptee_t};
             rhs=value})
      | _ -> failwith ("arrays must be compared to arrays or ptrs: " ^
                       (ttype_to_str rhs.t) ^ " <> " ^
                       (ttype_to_str lhs.t))
    end
  | Array _, _ -> begin match lhs.t, lhs.v with
      | Array ptee_t, Array cells ->
        List.mapi cells ~f:(fun idx value ->
            {lhs=value;
             rhs={v=Deref {v=Bop (Add, rhs, {v=Int idx;t=Uint32});t=rhs.t};
                  t=ptee_t}})
      | _ ->
        failwith ("arrays must be compared to arrays \
                   (ptrs not implemented yet): " ^
                  (ttype_to_str rhs.t) ^ " <> " ^
                  (ttype_to_str lhs.t))
    end
  | Ptr ptee_t, Addr pointee ->
    gen_plain_equalities {lhs={v=Deref lhs;t=ptee_t};
                          rhs=pointee}
  | Str (_, fields), Struct (_, fvals) ->
    List.join
      (List.map fields ~f:(fun (name,ttype) ->
           let v = List.find_exn fvals ~f:(fun {name=vname;_} ->
               String.equal vname name)
           in
           gen_plain_equalities
             {lhs={v=Str_idx (lhs, name);t=ttype};
              rhs=v.value}))
  | Str (_, fields), _ ->
    List.join
      (List.map fields ~f:(fun (name,ttype) ->
           gen_plain_equalities
             {lhs={v=Str_idx (lhs, name);t=ttype};
              rhs={v=Str_idx (rhs, name);t=ttype}}))
  | Sint64, _
  | Sint32, _
  | Sint16, _
  | Sint8, _
  | Uint64, _
  | Uint32, _
  | Uint16, _
  | Uint8,  _
  | Boolean, Id _
  | Boolean, Bool _
  | Ptr _, Id _
  | Ptr _, Int _ -> [{lhs;rhs={v=Cast(lhs.t,rhs);t=lhs.t}}]
  | Boolean, Int 0 ->
    [{lhs;rhs={v=Bool false;
               t=Boolean}}]
  | Boolean, Int 1 ->
    [{lhs;rhs={v=Bool true;
               t=Boolean}}]
  | Boolean, Int x ->
    [{lhs;rhs={v=Not {v=Bop (Eq, {v=Int x;t=Sint32},
                             {v=Int 0;t=Sint32});
                      t=Boolean};
               t=Boolean}}]
  | Ptr _, Zeroptr -> []
  | _ -> match lhs.v, rhs.v with
         | Deref lref, Deref rref -> gen_plain_equalities {lhs=lref; rhs=rref}
         | Id x, Deref {v=Addr {v=Id y;t=_};t=_} when x = y -> [{lhs;rhs}]
         | _ -> failwith ("unsupported output type:rhs.t=" ^
                          (ttype_to_str rhs.t) ^
                          " : rhs=" ^
                          (render_tterm rhs) ^
                          " : lhs.t=" ^
                          (ttype_to_str lhs.t) ^
                          " : lhs=" ^
                          (render_tterm lhs))

let gen_plain_equalities_for_all equalities =
  List.join (List.map equalities ~f:gen_plain_equalities)

let render_extra_pre_conditions context =
  String.concat ~sep:"\n"
    (List.map
       (* (gen_plain_equalities_for_all context.extra_pre_conditions)
          ^-- this should be done by now as a byproduct of filtering *)
       context.extra_pre_conditions
       ~f:(fun eq_cond ->
           (render_assignment eq_cond)))

let render_hist_fun_call {context;result} =
  "// PRECONDITIONS\n" ^
  (render_extra_pre_conditions context) ^
  "// PRELEMMAS AND CALL\n" ^
  (render_fcall_with_prelemmas context) ^
  "// RET STUFF\n" ^
  (match result.ret_val.t with
   | Ptr _ -> (if result.ret_val.v = Zeroptr then
                 "//@ assume(" ^ (Option.value_exn context.ret_name) ^
                 " == " ^ "0);\n"
               else
                 "//@ assume(" ^ (Option.value_exn context.ret_name) ^
                 " != " ^ "0);\n") ^
              "/* Do not render the return ptee assumption for hist calls */\n"
   | _ -> render_ret_equ_sttmt
            ~is_assert:false
            context.ret_name
            context.ret_type
            result.ret_val) ^
  "// POSTLEMMAS\n" ^
  (render_postlemmas context) ^ (* postlemmas can depend on the return value *)
  "// POSTCONDITIONS\n" ^ (* Postconditions can depend on post lemmas, e.g.
                             if the the post lemma "close_struct"*)
  (render_args_post_conditions ~is_assert:false
     (List.join (List.map result.args_post_conditions
                   ~f:gen_plain_equalities) )) ^
  (* ret can influence whether args are accessible *)
  (render_post_assumptions result.post_statements)

let gen_ret_equalities ret_val ret_name ret_type =
  match ret_name with
  | Some ret_name ->
    gen_plain_equalities {lhs={v=Id ret_name;t=ret_type};rhs=ret_val}
  | None -> []

let make_assignments_for_eqs equalities =
  List.map equalities ~f:(fun {lhs;rhs} ->
      {lhs=rhs;rhs=lhs})

let split_assignments assignments =
  let rec unfold_casts v = match v with
  | Cast (_, {v=v2;t=_}) -> unfold_casts v2
  | Bop (Bit_and, v2, {v=Int 65535;t=_} ) -> unfold_casts v2.v
  | _ -> v
  in
  List.fold assignments ~init:([],[]) ~f:(fun (concrete,symbolic) assignment ->
      match unfold_casts assignment.lhs.v with
      | Id _ -> (concrete,assignment::symbolic)
      | Int _ -> (assignment::concrete,symbolic)
      | Bool _ -> (assignment::concrete,symbolic)
      | Bop (Add, ({v=Id _;t=_} as symb), ({v=Int _;t=_} as delta)) ->
        (concrete,
         {lhs=symb;
          rhs={v=Bop (Sub, assignment.rhs, delta);
               t=assignment.rhs.t}}::
         symbolic)
      | Bop (Add, {v=Int delta;t=Sint32}, ({v=Id _;t=_} as symb))
        when delta < 0 ->
        (concrete,
         {lhs=symb;
          rhs={v=Bop (Add, assignment.rhs, {v=Int (-delta);t=Sint32});
               t=assignment.rhs.t}}::
         symbolic)
      | Bop (Sub, ({v=Id _;t=_} as symb), {v=Int delta;t=Sint32})
        when 0 <= delta ->
        (concrete,
         {lhs=symb;
          rhs={v=Bop (Add, assignment.rhs, {v=Int delta;t=Sint32});
               t=assignment.rhs.t}}::
         symbolic)
      | Bop (Sub, {v=Id _;t=_}, _) ->
        (assignment::concrete,symbolic)
      | Bop (Sub, {v=Int _;t=_}, _) ->
        (assignment::concrete,symbolic)
      | Struct (_, []) -> (* printf "skipping empty assignment: %s = %s" *)
        (*  (render_tterm assignment.lhs) *)
        (*  (render_tterm assignment.rhs); *)
        (concrete,symbolic)
      | Str_idx _ -> (assignment::concrete,symbolic)
      | Deref _ -> (assignment::concrete,symbolic)
      | _ -> failwith ("unsupported assignment in split_assignments: " ^
                       (render_tterm assignment.lhs) ^
                       " = " ^ (render_tterm assignment.rhs)))

let apply_assignments assignments terms =
  List.fold assignments ~init:terms ~f:(fun terms {lhs;rhs} ->
      List.map terms ~f:(replace_tterm lhs rhs))


let ids_from_term term =
  String.Set.of_list
    (collect_nodes (function
         | {v=Id name;t=_} -> Some name
         | _ -> None)
        term)

let ids_from_terms terms =
  List.fold terms ~init:String.Set.empty ~f:(fun ids term ->
      String.Set.union ids (ids_from_term term))

let ids_from_eq_conditions eq_conds =
  List.fold eq_conds ~init:String.Set.empty ~f:(fun ids cond ->
      String.Set.union (ids_from_term cond.lhs)
        (String.Set.union (ids_from_term cond.rhs) ids))

let split_constraints tterms symbols =
  List.partition_tf tterms ~f:(fun tterm ->
      Set.for_all (ids_from_term tterm) ~f:(String.Set.mem symbols))

let render_concrete_assignments_as_assertions assignments =
  String.concat ~sep:"\n"
    (List.map assignments ~f:(fun {lhs;rhs} ->
         match rhs.t with
         | Ptr _ ->
           "/*@ assert(" ^ (render_tterm rhs) ^ " == " ^
           (render_tterm lhs) ^ "); @*/"
         | _ ->
           "/*@ assert(" ^ (render_tterm lhs) ^ " == " ^
           (render_tterm rhs) ^ "); @*/"))

let expand_conjunctions terms =
  let rec expand_tterm tterm =
    match tterm with
    | {v=Bop (And, t1, t2);t=_} -> (expand_tterm t1) @ (expand_tterm t2)
    | tterm -> [tterm]
  in
  List.join (List.map terms ~f:expand_tterm)

let fix_constraints tterms =
  let res = List.map tterms ~f:(fun t1 -> match t1.v with
      | Bop (Eq,
             {v=Bool false;t=_},
             {v=Bop (Eq, {v=Int 0;t=Boolean}, real_value);t=_}) ->
        expand_conjunctions [real_value]
      | _ ->  [t1]) in
  List.join res

let bubble_equalities tterms =
  List.sort tterms ~compare:(fun t1 t2 ->
      match (t1.v,t2.v) with
      | (Bop (Eq,_,_), Bop (Eq,_,_)) -> 0
      | (Bop (Eq,_,_), _) -> -1
      | (_, Bop(Eq,_,_)) -> 1
      | (_,_) -> 0)

let is_there_device_constraint constraints =
  List.exists constraints ~f:(fun tterm ->
      match tterm.v with
      | Bop (Eq, _, {v=Id "device_0"; _}) -> true
      | _ -> false)

let guess_support_assignments constraints symbs =
  let constraints = fix_constraints constraints in
  (* printf "guess constraints\n";
  List.iter constraints ~f:(fun xxx -> printf "%s\n" (render_tterm xxx));
  printf "symbols:\n";
  Set.iter symbs ~f:(fun name -> printf "%s\n" name;); *)
  let there_is_a_device_constraint = is_there_device_constraint constraints in
  let (assignments,_) =
    List.fold (bubble_equalities constraints) 
      ~init:([],symbs)
      ~f:(fun (assignments,symbs) tterm ->
          (* printf "considering %s\n" (render_tterm tterm); *)
          match tterm.v with
          | Bop (Eq, {v=Id x;t}, rhs) when String.Set.mem symbs x ->
            (* printf "match 1st %s: %s\n" x (ttype_to_str t); *)
            ({lhs={v=Id x;t};rhs}::assignments, String.Set.remove symbs x)
          | Bop (Eq, lhs, {v=Id x;t}) when String.Set.mem symbs x ->
            (* printf "match 2nd %s: %s\n" x (ttype_to_str t); *)
            ({lhs={v=Id x;t};rhs=lhs}::assignments, String.Set.remove symbs x)
          | Bop (Le, {v=Int i;t=lt}, {v=Id x;t}) when String.Set.mem symbs x ->
            (* Stupid hack. If the variable is constrained to not be equal to
               another variable, we assume they have the same lower bound and
               assign the second one to bound+2 *)
            if List.exists constraints ~f:(fun cstr ->
                match cstr with
                | {v=Bop (Eq,
                          {v=Bool false;_},
                          {v=Bop (Eq,{v=Id _;_},{v=Id r;_});_});_}
                  when r = x -> true
                | _ -> false) then
              ({lhs={v=Id x;t};rhs={v=Int (i+2);t=lt}}::assignments,
               String.Set.remove symbs x)
            else if there_is_a_device_constraint then
              (*Dirty hack for a difficult case, analyzed by hand*)
              ({lhs={v=Id x;t};rhs={v=Int 1;t=lt}}::assignments,
               String.Set.remove symbs x)
            else
              ({lhs={v=Id x;t};rhs={v=Int i;t=lt}}::assignments,
               String.Set.remove symbs x)
          | Bop (Le, {v=Id x;t}, rhs) when String.Set.mem symbs x ->
            ({lhs={v=Id x;t};rhs}::assignments, String.Set.remove symbs x)
          | _ -> (assignments, symbs))
  in
  assignments

let ids_from_varspecs_map vars =
  List.fold (Map.data vars) ~init:String.Set.empty ~f:(fun acc var ->
      String.Set.add acc var.name)

let simplify_assignments assignments =
  List.fold assignments ~init:assignments ~f:(fun acc assignment ->
      List.map acc ~f:(fun {lhs;rhs} ->
          {lhs;rhs = replace_tterm assignment.lhs assignment.rhs rhs}))

let fix_mistyped_tip_ret tterm =
  match tterm.v with
  | Id name when String.equal name "tip_ret"
    -> {v=Not {v=Bop (Eq,{v=Int 0;t=Sint32},tterm);
               t=Boolean};
        t=Boolean}
  | Bop (Bit_and, _, _) ->
    {v=Not {v=Bop (Eq,{v=Int 0;t=Sint32},tterm);t=Boolean};t=Boolean}
  | _ -> tterm


let output_check_and_assignments
    ret_val ret_name ret_type model_constraints
    hist_symbs args_post_conditions cmplxs
  =
  let (input_constraints,output_constraints) =
    split_constraints model_constraints hist_symbs
  in
  let ret_equalities = gen_ret_equalities ret_val ret_name ret_type
  in
  let args_equalities =
    gen_plain_equalities_for_all args_post_conditions
  in
  let assignments = make_assignments_for_eqs (ret_equalities@args_equalities)
  in
  let output_constraints = expand_conjunctions output_constraints in
  let output_symbs = ids_from_eq_conditions assignments in
  let cmplx_symbs = ids_from_varspecs_map cmplxs in
  let unalloc_symbs =
    String.Set.diff
      (String.Set.diff
         (String.Set.diff
            (ids_from_terms output_constraints)
            output_symbs)
         hist_symbs)
      cmplx_symbs
  in
  let support_assignments =
    [](* guess_support_assignments output_constraints unalloc_symbs *)
  in
  let assignments =
    gen_plain_equalities_for_all (assignments@support_assignments)
  in
  let (concrete_assignments,
       symbolic_var_assignments) = split_assignments assignments
  in
  let symbolic_var_assignments =
    simplify_assignments symbolic_var_assignments
  in
  (* printf "substitution schema:\n"; *)
  (* List.iter symbolic_var_assignments ~f:(fun {lhs;rhs} -> *)
  (*     printf "%s = %s\n" (render_tterm lhs) (render_tterm rhs)); *)
  let upd_model_constraints =
    apply_assignments symbolic_var_assignments output_constraints
  in
  let output_check =
    "// Output check\n" ^
    "// Input assumptions\n" ^
    (render_conditions input_constraints ~is_assert:false) ^ "\n" ^
    "// Concrete equalities: \n" ^
    (render_concrete_assignments_as_assertions concrete_assignments) ^ "\n" ^
    "// Model constraints: \n" ^
    (String.concat ~sep:"\n"
       (List.map upd_model_constraints
          ~f:(fun constr ->
              match (fix_mistyped_tip_ret constr) with
              | {v=Bop (Eq, lhs, rhs);t=_} ->
                render_eq_sttmt ~is_assert:true lhs rhs
              | c -> "/*@ assert(" ^ (render_tterm c) ^ "); @*/")))
  in
  (output_check, symbolic_var_assignments)

let eq_cond_to_tterm {lhs;rhs} =
  {v=Bop (Eq, lhs, rhs);t=Boolean}

let render_context_condition conditions =
  match conditions with
  | [] -> "true"
  | _ -> String.concat ~sep:" && "
           (List.map conditions
              ~f:(fun x -> render_tterm x))

type rendered_result =
  { conditions: tterm list;
    output_check: string;
    args_conditions: string; }

let render_post_assertions results ret_name ret_type hist_symbs cmplxs =
  let render_context_conditions results =
    let rec do_render results =
      match results with
      | res :: tl ->
        "if (" ^ (render_context_condition res.conditions) ^ ") {\n" ^
          res.args_conditions ^ "\n" ^
          res.output_check ^ "\n" ^ 
        "} else " ^
        (do_render tl)
      | [] -> "{\n//@ assert(false);\n}\n"
    in
    let render_result result = 
      let (output_check, assignments) =
        output_check_and_assignments
          result.ret_val ret_name ret_type
          result.post_statements hist_symbs
          result.args_post_conditions cmplxs in
      let conditions =
        (* result.post_statements@ *)(List.map assignments ~f:eq_cond_to_tterm)
      in
      let args_post_conds_tterms =
        List.map result.args_post_conditions ~f:eq_cond_to_tterm
      in
      let conditions =
        List.filter conditions ~f:(fun s ->
            not(List.exists args_post_conds_tterms
                  ~f:(fun c -> term_eq s.v c.v)))
      in
      let args_conditions =
        render_args_post_conditions ~is_assert:true result.args_post_conditions
      in
      {conditions;output_check;args_conditions}
    in
    let rendered_results = List.map results ~f:render_result in
    let condition_sets =
      List.map rendered_results ~f:(fun r -> Set.Poly.of_list r.conditions)
    in
    let common_conditions =
      match List.reduce condition_sets ~f:Set.inter with
      | Some conds -> Set.to_list conds
      | None -> failwith "Not possible, there must be at least one result"
    in
    (render_conditions common_conditions ~is_assert:false) ^ "\n" ^
    (do_render rendered_results)
  in
  let rec render_ret_conditions groups =
    match groups with
    | (retval, results) :: tl ->
      let cond = begin match ret_name with
      | Some ret_name -> ret_name ^ " == " ^ (render_term retval)
      | None -> "true" end in
        "if (" ^ cond ^ ") {\n" ^
          (render_context_conditions results) ^
        "} else " ^
        (render_ret_conditions tl)
    | [] -> "{\n//@ assert(false);\n}\n"
  in
  (* We only have different return values when the return values are constants,
     e.g. 0 or 1. *)
  let all_const = List.for_all results ~f:(fun r -> is_constt r.ret_val) in
  let none_const =
    List.for_all results ~f:(fun r -> not(is_constt r.ret_val))
  in
  assert(all_const || none_const);
  if all_const then
    let groups =
      Map.Poly.of_alist_multi (List.map results ~f:(fun r -> (r.ret_val.v, r)))
    in
    (render_ret_conditions (Map.to_alist groups))
  else
    let all_same =
      List.for_all results ~f:(fun r ->
          term_eq r.ret_val.v (List.nth_exn results 0).ret_val.v)
    in
    if all_same then (render_context_conditions results)
    else failwith "expected all non-constant retvals to be the same"

let render_export_point name =
  "int " ^ name ^ ";\n"

let render_assignments args =
  String.concat ~sep:"\n"
    (List.join
       (List.map args ~f:(fun arg ->
            List.map (if arg.value.v = Undef then []
                      else
                        gen_plain_equalities
                          {lhs={v=Id arg.name;t=arg.value.t;};
                           rhs=arg.value})
              ~f:render_assignment)))

let render_tip_fun_call
    {context;results}
    export_point hist_symbols
    ~render_assertions
    cmplxs =
  (render_extra_pre_conditions context) ^ "\n" ^
  (render_fcall_with_prelemmas context) ^
  (render_postlemmas context) ^
  (render_export_point export_point) ^
  (if render_assertions then
     render_post_assertions
       results context.ret_name context.ret_type hist_symbols cmplxs
   else
     "")

let render_semantic_checks semantic_checks =
  if (String.equal semantic_checks "") then
    "\n// No semantic checks for this trace\n"
  else
    "\n// Semantics checks (rendered before the final call,\n" ^
    "// because the final call should be the invariant_consume)\n" ^
    "/*@ {\n" ^ semantic_checks ^ "\n} @*/\n"


let render_vars_declarations ( vars : var_spec list ) =
  String.concat ~sep:"\n"
    (List.map vars ~f:(fun v ->
         match v.value.t with
         | Array at -> (ttype_to_str at) ^ " " ^ v.name ^ "[];"
         | Unknown | Sunknown | Uunknown ->
           failwith ("Cannot render var decl '" ^
                     v.name ^ "' for type " ^ (ttype_to_str v.value.t))
         | _ -> ttype_to_str v.value.t ^ " " ^ v.name ^ ";")) ^ "\n"

let render_hist_calls hist_funs =
  String.concat ~sep:"\n" (List.map hist_funs ~f:render_hist_fun_call)

let render_cmplexes cmplxes =
  String.concat ~sep:"\n" (List.map (String.Map.data cmplxes) ~f:(fun var ->
      match var.value.t with
      | Array at ->
        (ttype_to_str at) ^ " " ^ var.name ^
        "[]; //" ^ (render_tterm var.value)
      | _ -> (ttype_to_str var.value.t) ^ " " ^
             var.name ^ "; //" ^ (render_tterm var.value))) ^ "\n"

let render_allocated_args args =
  String.concat ~sep:"\n"
    (List.map args
       ~f:(fun spec -> (ttype_to_str spec.value.t) ^ " " ^
                       (spec.name) ^ ";")) ^ "\n"

let render_args_hack args =
  (* Render declarations first, VeriFast complains otherwise *)
  String.concat ~sep:"\n"
    (List.map (List.filter args ~f:(fun spec -> is_pointer_t spec.value.t))
       ~f:(fun spec -> (ttype_to_str spec.value.t) ^ " " ^
                       (spec.name) ^ "bis;")) ^ "\n" ^
  (* Then the assignments *)
  String.concat ~sep:"\n"
    (List.map (List.filter args ~f:(fun spec -> is_pointer_t spec.value.t))
       ~f:(fun spec -> (spec.name) ^ " = " ^ (spec.name) ^ "bis;\n" ^
                       "*(&(" ^ (spec.name) ^ ")) = " ^
                       (spec.name) ^ "bis;")) ^ "\n" ^
  (* Then the assumptions, which would be overwritten by assignments otherwise
     *)
  String.concat ~sep:"\n"
    (List.map (List.filter args ~f:(fun spec -> is_pointer_t spec.value.t))
       ~f:(fun spec -> "//@ assume(" ^ (spec.name) ^ " == " ^
                       (spec.name) ^ "bis);")) ^ "\n"

let render_final finishing ~catch_leaks =
  if finishing && catch_leaks then
    "/* This sequence must terminate cleanly: no need for assume(false); */\n"
  else
    "//@ assume(false);\n"

let get_all_symbols calls =
  List.fold calls ~init:String.Set.empty ~f:(fun symbols call ->
      String.Set.union (ids_from_term call.result.ret_val)
        (String.Set.union (ids_from_eq_conditions
                             call.result.args_post_conditions)
           (String.Set.union (ids_from_eq_conditions
                                call.context.extra_pre_conditions)
              (String.Set.union (ids_from_term {v=call.context.application;
                                                t=Unknown})
                 (match call.context.ret_name with
                  | Some name -> (String.Set.add symbols name)
                  | None -> symbols)))))

let discard_redundant_preconditions ir =
  let filter_redundant_preconditions prev_postconds preconds =
    List.filter preconds ~f:(fun precond ->
        not (List.exists prev_postconds
               ~f:(fun postcond ->
                   Ir.term_eq precond.lhs.v postcond.lhs.v &&
                   Ir.term_eq precond.rhs.v postcond.rhs.v)))
  in
  let (last_postconds, hist_calls) =
    List.fold_map ir.hist_calls ~init:[] ~f:(fun last_postconds hist_call ->
        (gen_plain_equalities_for_all hist_call.result.args_post_conditions,
         {context = {hist_call.context with
                     extra_pre_conditions =
                       filter_redundant_preconditions
                         last_postconds
                         (gen_plain_equalities_for_all
                            hist_call.context.extra_pre_conditions)};
          result = hist_call.result}))
  in
  let tip_call = {context = {ir.tip_call.context with
                             extra_pre_conditions =
                               filter_redundant_preconditions
                                 last_postconds
                                 (gen_plain_equalities_for_all
                                    ir.tip_call.context.extra_pre_conditions)};
                  results = ir.tip_call.results}
  in
  {ir with hist_calls; tip_call}

let render_ir ir fout ~render_assertions =
  let ir = discard_redundant_preconditions ir in
  let hist_symbols = get_all_symbols ir.hist_calls in
  Out_channel.with_file fout ~f:(fun cout ->
      Out_channel.output_string cout ir.preamble;
      Out_channel.output_string cout (render_cmplexes ir.cmplxs);
      Out_channel.output_string cout (render_vars_declarations
                                        (String.Map.data ir.free_vars));
      Out_channel.output_string cout (render_allocated_args ir.arguments);
      Out_channel.output_string cout (render_args_hack ir.arguments);
      Out_channel.output_string cout (render_conditions
                                        ir.context_assumptions
                                        ~is_assert:false);
      Out_channel.output_string cout (render_assignments ir.arguments);
      (* Yes, this is stupid; but right before a deadline I will not spend hours
         refactoring import.ml to fix it. *)
      let stupid_fix =
        Str.global_replace
          (Str.regexp_string "(next_time_0 - vector_data_reset_1_8)")
          "(uint64_t)(next_time_0 - vector_data_reset_1_8)"
      in
      Out_channel.output_string cout (stupid_fix (render_hist_calls
                                                    ir.hist_calls));
      Out_channel.output_string cout (render_semantic_checks ir.semantic_checks);
      Out_channel.output_string cout (stupid_fix (render_tip_fun_call
                                        ir.tip_call ir.export_point
                                        hist_symbols
                                        ~render_assertions
                                        ir.cmplxs));
      Out_channel.output_string cout (render_final ir.finishing
                                        ~catch_leaks:render_assertions);
      Out_channel.output_string cout "}\n")
