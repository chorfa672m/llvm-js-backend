; RUN: llc < %s -march=arm -mattr=+vfp2 | FileCheck %s

define i32 @f1(float %a) {
;CHECK: f1:
;CHECK: fcmpes
;CHECK: movmi
entry:
        %tmp = fcmp olt float %a, 1.000000e+00          ; <i1> [#uses=1]
        %tmp1 = zext i1 %tmp to i32              ; <i32> [#uses=1]
        ret i32 %tmp1
}

define i32 @f2(float %a) {
;CHECK: f2:
;CHECK: fcmpes
;CHECK: moveq
entry:
        %tmp = fcmp oeq float %a, 1.000000e+00          ; <i1> [#uses=1]
        %tmp2 = zext i1 %tmp to i32              ; <i32> [#uses=1]
        ret i32 %tmp2
}

define i32 @f3(float %a) {
;CHECK: f3:
;CHECK: fcmpes
;CHECK: movgt
entry:
        %tmp = fcmp ogt float %a, 1.000000e+00          ; <i1> [#uses=1]
        %tmp3 = zext i1 %tmp to i32              ; <i32> [#uses=1]
        ret i32 %tmp3
}

define i32 @f4(float %a) {
;CHECK: f4:
;CHECK: fcmpes
;CHECK: movge
entry:
        %tmp = fcmp oge float %a, 1.000000e+00          ; <i1> [#uses=1]
        %tmp4 = zext i1 %tmp to i32              ; <i32> [#uses=1]
        ret i32 %tmp4
}

define i32 @f5(float %a) {
;CHECK: f5:
;CHECK: fcmpes
;CHECK: movls
entry:
        %tmp = fcmp ole float %a, 1.000000e+00          ; <i1> [#uses=1]
        %tmp5 = zext i1 %tmp to i32              ; <i32> [#uses=1]
        ret i32 %tmp5
}

define i32 @f6(float %a) {
;CHECK: f6:
;CHECK: fcmpes
;CHECK: movne
entry:
        %tmp = fcmp une float %a, 1.000000e+00          ; <i1> [#uses=1]
        %tmp6 = zext i1 %tmp to i32              ; <i32> [#uses=1]
        ret i32 %tmp6
}

define i32 @g1(double %a) {
;CHECK: g1:
;CHECK: fcmped
;CHECK: movmi
entry:
        %tmp = fcmp olt double %a, 1.000000e+00         ; <i1> [#uses=1]
        %tmp7 = zext i1 %tmp to i32              ; <i32> [#uses=1]
        ret i32 %tmp7
}
