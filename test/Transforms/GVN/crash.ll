; RUN: opt -gvn %s -disable-output

; PR5631

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"
target triple = "x86_64-apple-darwin10.0"

define i32* @peel_to_type(i8* %name, i32 %namelen, i32* %o, i32 %expected_type) nounwind ssp {
entry:
  br i1 undef, label %if.end13, label %while.body.preheader


if.end13:                                         ; preds = %if.then6
  br label %while.body.preheader

while.body.preheader:                             ; preds = %if.end13, %if.end
  br label %while.body

while.body:                                       ; preds = %while.body.backedge, %while.body.preheader
  %o.addr.0 = phi i32* [ undef, %while.body.preheader ], [ %o.addr.0.be, %while.body.backedge ] ; <i32*> [#uses=2]
  br i1 false, label %return.loopexit, label %lor.lhs.false

lor.lhs.false:                                    ; preds = %while.body
  %tmp20 = bitcast i32* %o.addr.0 to i32*         ; <i32*> [#uses=1]
  %tmp22 = load i32* %tmp20                       ; <i32> [#uses=0]
  br i1 undef, label %land.lhs.true24, label %if.end31

land.lhs.true24:                                  ; preds = %lor.lhs.false
  %call28 = call i32* @parse_object(i8* undef) nounwind ; <i32*> [#uses=0]
  br i1 undef, label %return.loopexit, label %if.end31

if.end31:                                         ; preds = %land.lhs.true24, %lor.lhs.false
  br i1 undef, label %return.loopexit, label %if.end41

if.end41:                                         ; preds = %if.end31
  %tmp43 = bitcast i32* %o.addr.0 to i32*         ; <i32*> [#uses=1]
  %tmp45 = load i32* %tmp43                       ; <i32> [#uses=0]
  br i1 undef, label %if.then50, label %if.else

if.then50:                                        ; preds = %if.end41
  %tmp53 = load i32** undef                       ; <i32*> [#uses=1]
  br label %while.body.backedge

if.else:                                          ; preds = %if.end41
  br i1 undef, label %if.then62, label %if.else67

if.then62:                                        ; preds = %if.else
  br label %while.body.backedge

while.body.backedge:                              ; preds = %if.then62, %if.then50
  %o.addr.0.be = phi i32* [ %tmp53, %if.then50 ], [ undef, %if.then62 ] ; <i32*> [#uses=1]
  br label %while.body

if.else67:                                        ; preds = %if.else
  ret i32* null

return.loopexit:                                  ; preds = %if.end31, %land.lhs.true24, %while.body
  ret i32* undef
}

declare i32* @parse_object(i8*)






%struct.attribute_spec = type { i8*, i32, i32, i8, i8, i8 }

@attribute_tables = external global [4 x %struct.attribute_spec*] ; <[4 x %struct.attribute_spec*]*> [#uses=2]

define void @decl_attributes() nounwind {
entry:
  br label %bb69.i

bb69.i:                                           ; preds = %bb57.i.preheader
  %tmp4 = getelementptr inbounds [4 x %struct.attribute_spec*]* @attribute_tables, i32 0, i32 undef ; <%struct.attribute_spec**> [#uses=1]
  %tmp3 = load %struct.attribute_spec** %tmp4, align 4 ; <%struct.attribute_spec*> [#uses=1]
  br label %bb65.i

bb65.i:                                           ; preds = %bb65.i.preheader, %bb64.i
  %storemerge6.i = phi i32 [ 1, %bb64.i ], [ 0, %bb69.i ] ; <i32> [#uses=3]
  %scevgep14 = getelementptr inbounds %struct.attribute_spec* %tmp3, i32 %storemerge6.i, i32 0 ; <i8**> [#uses=1]
  %tmp2 = load i8** %scevgep14, align 4           ; <i8*> [#uses=0]
  %tmp = load %struct.attribute_spec** %tmp4, align 4 ; <%struct.attribute_spec*> [#uses=1]
  %scevgep1516 = getelementptr inbounds %struct.attribute_spec* %tmp, i32 %storemerge6.i, i32 0 ; <i8**> [#uses=0]
  unreachable

bb64.i:                                           ; Unreachable
  br label %bb65.i

bb66.i:                                           ; Unreachable
  br label %bb69.i
}



; rdar://7438974

@g = external global i64, align 8

define i32* @foo() {
do.end17.i:
  %tmp18.i = load i7** undef
  %tmp1 = bitcast i7* %tmp18.i to i8*
  br i1 undef, label %do.body36.i, label %if.then21.i

if.then21.i:
  %tmp2 = bitcast i7* %tmp18.i to i8*
  ret i32* undef

do.body36.i:
  %ivar38.i = load i64* @g 
  %tmp3 = bitcast i7* %tmp18.i to i8*
  %add.ptr39.sum.i = add i64 %ivar38.i, 8
  %tmp40.i = getelementptr inbounds i8* %tmp3, i64 %add.ptr39.sum.i
  %tmp4 = bitcast i8* %tmp40.i to i64*
  %tmp41.i = load i64* %tmp4
  br i1 undef, label %if.then48.i, label %do.body57.i

if.then48.i:
  %call54.i = call i32 @foo2()
  br label %do.body57.i

do.body57.i:
  %tmp58.i = load i7** undef
  %ivar59.i = load i64* @g
  %tmp5 = bitcast i7* %tmp58.i to i8*
  %add.ptr65.sum.i = add i64 %ivar59.i, 8
  %tmp66.i = getelementptr inbounds i8* %tmp5, i64 %add.ptr65.sum.i
  %tmp6 = bitcast i8* %tmp66.i to i64*
  %tmp67.i = load i64* %tmp6
  ret i32* undef
}

declare i32 @foo2()

