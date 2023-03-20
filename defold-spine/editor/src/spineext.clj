;; Copyright 2020 The Defold Foundation
;; Licensed under the Defold License version 1.0 (the "License"); you may not use
;; this file except in compliance with the License.
;;
;; You may obtain a copy of the License, together with FAQs at
;; https://www.defold.com/license
;;
;; Unless required by applicable law or agreed to in writing, software distributed
;; under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
;; CONDITIONS OF ANY KIND, either express or implied. See the License for the
;; specific language governing permissions and limitations under the License.

(ns editor.spineext
  (:require [clojure.java.io :as io]
            [clojure.string :as str]
            [editor.protobuf :as protobuf]
            [dynamo.graph :as g]
            [util.murmur :as murmur]
            [editor.build-target :as bt]
            [editor.graph-util :as gu]
            [editor.geom :as geom]
            [editor.material :as material]
            [editor.math :as math]
            [editor.gl :as gl]
            [editor.gl.shader :as shader]
            [editor.gl.texture :as texture]
            [editor.gl.vertex :as vtx]
            [editor.defold-project :as project]
            [editor.resource :as resource]
            [editor.resource-node :as resource-node]
            [editor.scene-cache :as scene-cache] ; debug only
            [editor.scene-picking :as scene-picking]
            [editor.render :as render]
            [editor.validation :as validation]
            [editor.workspace :as workspace]
            [editor.gl.pass :as pass]
            [editor.types :as types]
            [editor.outline :as outline]
            [editor.properties :as properties]
            [editor.rig :as rig])
  (:import [com.dynamo.bob.textureset TextureSetGenerator$UVTransform]
           [com.dynamo.bob.util BezierUtil RigUtil$Transform]
           [editor.gl.shader ShaderLifecycle]
           [editor.types AABB]
           [com.jogamp.opengl GL GL2 GLContext]
           [org.apache.commons.io IOUtils]
           [java.io IOException]
           [java.util HashSet]
           [java.net URL]
           [javax.vecmath Matrix4d Vector3d Vector4d]))


(set! *warn-on-reflection* true)

(def spine-json-icon "/defold-spine/editor/resources/icons/32/Icons_16-Spine-scene.png")
(def spine-scene-icon "/defold-spine/editor/resources/icons/32/Icons_16-Spine-scene.png")
(def spine-model-icon "/defold-spine/editor/resources/icons/32/Icons_15-Spine-model.png")
(def spine-bone-icon "/defold-spine/editor/resources/icons/32/Icons_18-Spine-bone.png")
(def spine-material-path "/defold-spine/assets/spine.material")

(def spine-json-ext "spinejson")
(def spine-scene-ext "spinescene")
(def spine-model-ext "spinemodel")


; Plugin functions (from Spine.java)

;; (defn- debug-cls [^Class cls]
;;   (doseq [m (.getMethods cls)]
;;     (prn (.toString m))
;;     (println "Method Name: " (.getName m) "(" (.getParameterTypes m) ")")
;;     (println "Return Type: " (.getReturnType m) "\n")))

; More about JNA + Clojure
; https://nakkaya.com/2009/11/16/java-native-access-from-clojure/

(def spine-plugin-cls (workspace/load-class! "com.dynamo.bob.pipeline.Spine"))
(def spine-plugin-exception-cls (workspace/load-class! "com.dynamo.bob.pipeline.Spine$SpineException"))
(def spine-plugin-pointer-cls (workspace/load-class! "com.dynamo.bob.pipeline.Spine$SpinePointer"))
(def spine-plugin-aabb-cls (workspace/load-class! "com.dynamo.bob.pipeline.Spine$AABB"))
(def spine-plugin-blendmode-cls (workspace/load-class! "com.dynamo.spine.proto.Spine$SpineModelDesc$BlendMode"))
(def spine-plugin-spinescene-cls (workspace/load-class! "com.dynamo.spine.proto.Spine$SpineSceneDesc"))
(def spine-plugin-spinemodel-cls (workspace/load-class! "com.dynamo.spine.proto.Spine$SpineModelDesc"))

(def byte-array-cls (Class/forName "[B"))
(def float-array-cls (Class/forName "[F"))
(def string-array-cls (Class/forName "[Ljava.lang.String;"))

(defn- plugin-invoke-static [^Class cls name types args]
  (let [method (.getMethod cls name types)]
    (.invoke method nil (into-array Object args))))

(defn- plugin-load-file-from-buffer
  ; The instance is garbage collected by Java
  ([bytes-json path-json bytes-texture-set path-texture-set]
   (if (instance? byte-array-cls bytes-json)
     (plugin-invoke-static spine-plugin-cls "SPINE_LoadFileFromBuffer" (into-array Class [byte-array-cls String byte-array-cls String]) [bytes-json path-json bytes-texture-set path-texture-set])
     (throw (IOException. (str "Couldn't read data from " path-json)))))
  ([bytes-json path-json]
   (if (instance? byte-array-cls bytes-json)
     (plugin-invoke-static spine-plugin-cls "SPINE_LoadFileFromBuffer" (into-array Class [byte-array-cls String]) [bytes-json path-json])
     (throw (IOException. (str "Couldn't read data from " path-json))))))

(defn- plugin-get-animations [handle]
  (plugin-invoke-static spine-plugin-cls "SPINE_GetAnimations" (into-array Class [spine-plugin-pointer-cls]) [handle]))

;(defn- plugin-get-aabb ^"com.dynamo.bob.pipeline.Spine$AABB" [handle]
(defn- plugin-get-aabb [handle]
  (plugin-invoke-static spine-plugin-cls "SPINE_GetAABB" (into-array Class [spine-plugin-pointer-cls]) [handle]))

(defn plugin-set-skin [handle ^String skin]
  (plugin-invoke-static spine-plugin-cls "SPINE_SetSkin" (into-array Class [spine-plugin-pointer-cls String]) [handle skin]))

(defn plugin-set-animation [handle ^String animation]
  (let [valid-anims (vec (plugin-get-animations handle))]
    (when (contains? valid-anims animation)
      (plugin-invoke-static spine-plugin-cls "SPINE_SetAnimation" (into-array Class [spine-plugin-pointer-cls String]) [handle animation]))))

(defn plugin-update-vertices [handle dt]
  (plugin-invoke-static spine-plugin-cls "SPINE_UpdateVertices" (into-array Class [spine-plugin-pointer-cls Float/TYPE]) [handle (float dt)]))

;(defn- plugin-get-bones ^"[Lcom.dynamo.bob.pipeline.Spine$Bone;" [handle]
(defn plugin-get-bones [handle]
  (plugin-invoke-static spine-plugin-cls "SPINE_GetBones" (into-array Class [spine-plugin-pointer-cls]) [handle]))

(defn plugin-get-skins [handle]
  (plugin-invoke-static spine-plugin-cls "SPINE_GetSkins" (into-array Class [spine-plugin-pointer-cls]) [handle]))

;(defn- plugin-get-vertex-buffer-data ^"[Lcom.dynamo.bob.pipeline.Spine$RiveVertex;" [handle]
(defn plugin-get-vertex-buffer-data [handle]
  (plugin-invoke-static spine-plugin-cls "SPINE_GetVertexBuffer" (into-array Class [spine-plugin-pointer-cls]) [handle]))

;(defn- plugin-get-render-objects ^"[Lcom.dynamo.bob.pipeline.Spine$RenderObject;" [handle]
(defn- plugin-get-render-objects [handle]
  (plugin-invoke-static spine-plugin-cls "SPINE_GetRenderObjects" (into-array Class [spine-plugin-pointer-cls]) [handle]))


(set! *warn-on-reflection* false)

(defn- get-aabb [handle]
  (let [paabb (plugin-get-aabb handle)
        aabb (geom/coords->aabb [(.-minX paabb) (.-minY paabb) 0] [(.-maxX paabb) (.-maxY paabb) 0])]
    aabb))

(set! *warn-on-reflection* true)

(def ^:private ^TextureSetGenerator$UVTransform uv-identity (TextureSetGenerator$UVTransform.))

(defn- prop-resource-error [nil-severity _node-id prop-kw prop-value prop-name]
  (or (validation/prop-error nil-severity _node-id prop-kw validation/prop-nil? prop-value prop-name)
      (validation/prop-error :fatal _node-id prop-kw validation/prop-resource-not-exists? prop-value prop-name)))

(defn- validate-scene-atlas [_node-id atlas]
  (prop-resource-error :fatal _node-id :atlas atlas "Atlas"))

(defn- is-spine-scene-json-name? [resource prop-name]
  (let [path (resource/resource->proj-path resource)]
    (when (not (str/ends-with? path spine-json-ext))
      (format "%s file '%s' doesn't end with '.%s'" prop-name path spine-json-ext))))

(defn- validate-scene-spine-json [_node-id spine-json]
  (or (prop-resource-error :fatal _node-id :spine-json spine-json "Spine Json")
      (validation/prop-error :fatal _node-id :spine-json is-spine-scene-json-name? spine-json "Spine Json")))

;; (g/defnk produce-scene-build-targets
;;   [_node-id own-build-errors resource spine-scene-pb atlas dep-build-targets]
;;   (g/precluding-errors own-build-errors
;;                        (rig/make-rig-scene-build-targets _node-id
;;                                                          resource
;;                                                          (assoc spine-scene-pb
;;                                                                 :texture-set atlas)
;;                                                          dep-build-targets
;;                                                          [:texture-set])))

;; (defn- read-bones
;;   [spine-scene]
;;   (mapv (fn [b]
;;           {:id (murmur/hash64 (get b "name"))
;;            :name (get b "name")
;;            :parent (when (contains? b "parent") (murmur/hash64 (get b "parent")))
;;            :position [(get b "x" 0) (get b "y" 0) 0]
;;            :rotation (angle->clj-quat (get b "rotation" 0))
;;            :scale [(get b "scaleX" 1) (get b "scaleY" 1) 1]
;;            :inherit-scale (get b "inheritScale" true)
;;            :length (get b "length")})
;;         (get spine-scene "bones")))


(g/defnk produce-spine-scene-pb [_node-id spine-json atlas]
  {:spine_json (resource/resource->proj-path spine-json)
   :atlas (resource/resource->proj-path atlas)})

;; (defn- transform-positions [^Matrix4d transform mesh]
;;   (let [p (Point3d.)]
;;     (update mesh :positions (fn [positions]
;;                               (->> positions
;;                                 (partition 3)
;;                                 (mapcat (fn [[x y z]]
;;                                           (.set p x y z)
;;                                           (.transform transform p)
;;                                           [(.x p) (.y p) (.z p)])))))))

(shader/defshader spine-id-vertex-shader
  (attribute vec4 position)
  (attribute vec2 texcoord0)
  (varying vec2 var_texcoord0)
  (defn void main []
    (setq gl_Position (* gl_ModelViewProjectionMatrix position))
    (setq var_texcoord0 texcoord0)))

(shader/defshader spine-id-fragment-shader
  (varying vec2 var_texcoord0)
  (uniform sampler2D texture_sampler)
  (uniform vec4 id)
  (defn void main []
    (setq vec4 color (texture2D texture_sampler var_texcoord0.xy))
    (if (> color.a 0.05)
      (setq gl_FragColor id)
      (discard))))

(def spine-id-shader (shader/make-shader ::id-shader spine-id-vertex-shader spine-id-fragment-shader {"id" :id}))

(vtx/defvertex vtx-pos-tex-col
  (vec3 position)
  (vec2 texcoord0)
  (vec4 color))

(defn generate-vertex-buffer [verts]
  ; verts should be in the format [[x y z u v r g b a] [x y z...] ...]
  (let [vcount (count verts)]
    (when (> vcount 0)
      (let [vb (->vtx-pos-tex-col vcount)
            vb-out (persistent! (reduce conj! vb verts))]
        vb-out))))

(set! *warn-on-reflection* false)

(defn transform-vertices-as-vec [vertices]
  ; vertices is a SpineVertex array
  (map (fn [vert] [(.x vert) (.y vert) (.z vert) (.u vert) (.v vert) (.r vert) (.g vert) (.b vert) (.a vert)]) vertices))

(set! *warn-on-reflection* true)

(defn renderable->handle [renderable]
  (get-in renderable [:user-data :spine-data-handle]))

(defn renderable->render-objects [renderable]
  (let [handle (renderable->handle renderable)
        vb-data (plugin-get-vertex-buffer-data handle)
        vb-data-transformed (transform-vertices-as-vec vb-data)
        vb (generate-vertex-buffer vb-data-transformed)
        render-objects (plugin-get-render-objects handle)]
    {:vertex-buffer vb :render-objects render-objects :handle handle :renderable renderable}))


(defn collect-render-groups [renderables]
  (map renderable->render-objects renderables))

(def constant-tint (murmur/hash64 "tint"))

(defn- constant-hash->name [hash]
  (condp = hash
    constant-tint "tint"
    "unknown"))

(defn- do-mask [mask count]
  ; Checks if a bit in the mask is set: "(mask & (1<<count)) != 0"
  (not= 0 (bit-and mask (bit-shift-left 1 count))))

; See GetOpenGLCompareFunc in graphics_opengl.cpp
(defn- stencil-func->gl-func [^long func]
  (case func
    0 GL/GL_NEVER
    1 GL/GL_LESS
    2 GL/GL_LEQUAL
    3 GL/GL_GREATER
    4 GL/GL_GEQUAL
    5 GL/GL_EQUAL
    6 GL/GL_NOTEQUAL
    7 GL/GL_ALWAYS))

(defn- stencil-op->gl-op [^long op]
  (case op
    0 GL/GL_KEEP
    1 GL/GL_ZERO
    2 GL/GL_REPLACE
    3 GL/GL_INCR
    4 GL/GL_INCR_WRAP
    5 GL/GL_DECR
    6 GL/GL_DECR_WRAP
    7 GL/GL_INVERT))

(set! *warn-on-reflection* false)

(defn- set-stencil-func! [^GL2 gl face-type ref ref-mask state]
  (let [gl-func (stencil-func->gl-func (.m_Func state))
        op-stencil-fail (stencil-op->gl-op (.m_OpSFail state))
        op-depth-fail (stencil-op->gl-op (.m_OpDPFail state))
        op-depth-pass (stencil-op->gl-op (.m_OpDPPass state))]
    (.glStencilFuncSeparate gl face-type gl-func ref ref-mask)
    (.glStencilOpSeparate gl face-type op-stencil-fail op-depth-fail op-depth-pass)))

(set! *warn-on-reflection* true)

(defn- to-int [b]
  (bit-and 0xff (int b)))


(set! *warn-on-reflection* false)

; See ApplyStencilTest in render.cpp for reference
(defn- set-stencil-test-params! [^GL2 gl params]
  (let [clear (.m_ClearBuffer params)
        mask (to-int (.m_BufferMask params))
        color-mask (.m_ColorBufferMask params)
        separate-states (.m_SeparateFaceStates params)
        ref (to-int (.m_Ref params))
        ref-mask (to-int (.m_RefMask params))
        state-front (.m_Front params)
        state-back (if (not= separate-states 0) (.m_Back params) state-front)]
    (when (not= clear 0)
      (.glStencilMask gl 0xFF)
      (.glClear gl GL/GL_STENCIL_BUFFER_BIT))

    (.glColorMask gl (do-mask color-mask 3) (do-mask color-mask 2) (do-mask color-mask 1) (do-mask color-mask 0))
    (.glStencilMask gl mask)

    (set-stencil-func! gl GL/GL_FRONT ref ref-mask state-front)
    (set-stencil-func! gl GL/GL_BACK ref ref-mask state-back)))

(defn- set-constant! [^GL2 gl shader constant]
  (let [name-hash (.m_NameHash constant)]
    (when (not= name-hash 0)
      (let [v (.m_Value constant)
            value (Vector4d. (.x v) (.y v) (.z v) (.w v))]
        (shader/set-uniform shader gl (constant-hash->name name-hash) value)))))

(defn- set-constants! [^GL2 gl shader ro]
  (doall (map (fn [constant] (set-constant! gl shader constant)) (.m_Constants ro))))

(defn- do-render-object! [^GL2 gl render-args shader renderable ro]
  (let [start (.m_VertexStart ro) ; the name is from the engine, but in this case refers to the index
        count (.m_VertexCount ro)
        face-winding (if (not= (.m_FaceWindingCCW ro) 0) GL/GL_CCW GL/GL_CW)
        _ (set-constants! gl shader ro)
        ro-transform (double-array (.m (.m_WorldTransform ro)))
        renderable-transform (Matrix4d. (:world-transform renderable)) ; make a copy so we don't alter the original

        ro-matrix (doto (Matrix4d. ro-transform) (.transpose))
        shader-world-transform (doto renderable-transform (.mul ro-matrix))
        use-index-buffer (not= (.m_UseIndexBuffer ro) 0)
        triangle-mode (if (not= (.m_IsTriangleStrip ro) 0) GL/GL_TRIANGLE_STRIP GL/GL_TRIANGLES)
        render-args (merge render-args
                           (math/derive-render-transforms shader-world-transform
                                                          (:view render-args)
                                                          (:projection render-args)
                                                          (:texture render-args)))]

    (shader/set-uniform shader gl "world_view_proj" (:world-view-proj render-args))
    (when (not= (.m_SetFaceWinding ro) 0)
      (gl/gl-front-face gl face-winding))
    (when (not= (.m_SetStencilTest ro) 0)
      (set-stencil-test-params! gl (.m_StencilTestParams ro)))
    (when (not use-index-buffer)
      (gl/gl-draw-arrays gl triangle-mode start count))
    (when use-index-buffer
      (gl/gl-draw-elements gl triangle-mode start count))
    ; reset blend state
    (.glBlendFunc gl GL/GL_SRC_ALPHA GL/GL_ONE_MINUS_SRC_ALPHA)))

(set! *warn-on-reflection* true)

; Lent from gui_clipping.clj
(defn- setup-gl [^GL2 gl]
  (.glEnable gl GL/GL_STENCIL_TEST)
  (.glClear gl GL/GL_STENCIL_BUFFER_BIT))

(defn- restore-gl [^GL2 gl]
  (.glDisable gl GL/GL_STENCIL_TEST)
  (.glColorMask gl true true true true))


(defn- render-group-transparent [^GL2 gl render-args override-shader group]
  (let [renderable (:renderable group)
        user-data (:user-data renderable)
        blend-mode (:blend-mode user-data)
        gpu-texture (or (get user-data :gpu-texture) texture/white-pixel)
        shader (if (not= override-shader nil) override-shader (:shader user-data))
        vb (:vertex-buffer group)
        render-objects (:render-objects group)
        vertex-binding (vtx/use-with ::spine-trans vb shader)]
    (gl/with-gl-bindings gl render-args [gpu-texture shader vertex-binding]
      (setup-gl gl)
      (gl/set-blend-mode gl blend-mode)
      (doall (map (fn [ro] (do-render-object! gl render-args shader renderable ro)) render-objects))
      (restore-gl gl))))

(defn- render-spine-outlines [^GL2 gl render-args renderables rcount]
  (assert (= (:pass render-args) pass/outline))
  (render/render-aabb-outline gl render-args ::spine-outline renderables rcount))

(defn- render-spine-scenes [^GL2 gl render-args renderables rcount]
  (let [pass (:pass render-args)]
    (condp = pass
      pass/transparent
      (when-let [groups (collect-render-groups renderables)]
        (doall (map (fn [renderable] (render-group-transparent gl render-args nil renderable)) groups)))

      pass/selection
      (when-let [groups (collect-render-groups renderables)]
        (doall (map (fn [renderable] (render-group-transparent gl render-args spine-id-shader renderable)) groups))))))


;; (defn- render-spine-skeletons [^GL2 gl render-args renderables rcount]
;;   (assert (= (:pass render-args) pass/transparent))
;;   (when-let [vb (gen-skeleton-vb renderables)]
;;     (let [vertex-binding (vtx/use-with ::spine-skeleton vb render/shader-outline)]
;;       (gl/with-gl-bindings gl render-args [render/shader-outline vertex-binding]
;;         (gl/gl-draw-arrays gl GL/GL_LINES 0 (count vb))))))

(g/defnk produce-main-scene [_node-id aabb material-shader gpu-texture default-tex-params aabb spine-scene-pb spine-data-handle]
  (when (and gpu-texture)
    (let [blend-mode :blend-mode-alpha]
      (assoc {:node-id _node-id :aabb aabb}
             :renderable {:render-fn render-spine-scenes
                          :tags #{:spine}
                          :batch-key [gpu-texture material-shader]
                          :select-batch-key _node-id
                          :user-data {:aabb aabb
                                      :spine-scene-pb spine-scene-pb
                                      :spine-data-handle spine-data-handle
                                      :shader material-shader
                                      :gpu-texture gpu-texture
                                      :tex-params default-tex-params
                                      :blend-mode blend-mode}
                          :passes [pass/transparent pass/selection]}))))

(defn- make-spine-outline-scene [_node-id aabb]
  {:aabb aabb
   :node-id _node-id
   :renderable {:render-fn render-spine-outlines
                :tags #{:spine :outline}
                :batch-key ::outline
                :passes [pass/outline]}})

;;//////////////////////////////////////////////////////////////////////////////////////////////

(g/defnode SpineBone
  (inherits outline/OutlineNode)
  (property name g/Str (dynamic read-only? (g/constantly true)))
  (property position types/Vec3
            (dynamic edit-type (g/constantly (properties/vec3->vec2 0.0)))
            (dynamic read-only? (g/constantly true)))
  (property rotation g/Num (dynamic read-only? (g/constantly true)))
  (property scale types/Vec3
            (dynamic edit-type (g/constantly (properties/vec3->vec2 1.0)))
            (dynamic read-only? (g/constantly true)))
  (property length g/Num
            (dynamic read-only? (g/constantly true)))

  (input nodes g/Any :array)
  (input child-bones g/Any :array)
  (input child-outlines g/Any :array)

  (output transform Matrix4d :cached produce-transform)
  (output bone g/Any (g/fnk [name transform child-bones]
                            {:name name
                             :local-transform transform
                             :children child-bones}))
  (output node-outline outline/OutlineData (g/fnk [_node-id name child-outlines]
                                                  {:node-id _node-id
                                                   :node-outline-key name
                                                   :label name
                                                   :icon spine-bone-icon
                                                   :children child-outlines
                                                   :read-only true})))

;;//////////////////////////////////////////////////////////////////////////////////////////////

(set! *warn-on-reflection* false)

; Creates the bone hierarcy
(defn- is-root-bone? [bone]
  (= -1 (.-parent bone)))

(defn- create-bone [parent-id spine-bone]
  (let [name (.-name spine-bone)
        x (.-posX spine-bone)
        y (.-posY spine-bone)
        rotation (.-rotation spine-bone)
        scale-x (.-scaleX spine-bone)
        scale-y (.-scaleY spine-bone)
        length (.-length spine-bone)
        parent-graph-id (g/node-id->graph-id parent-id)
        bone-tx-data (g/make-nodes parent-graph-id [bone [SpineBone :name name :position [x y 0] :rotation rotation :scale [scale-x scale-y 1.0] :length length]]
                                   ; Hook this node into the parent's lists
                                   (g/connect bone :_node-id parent-id :nodes)
                                   (g/connect bone :node-outline parent-id :child-outlines)
                                   (g/connect bone :bone parent-id :child-bones))]
    bone-tx-data))

(defn- tx-first-created [tx-data]
  (get-in (first tx-data) [:node :_node-id]))

(defn- create-bone-hierarchy [parent-id bones bone]
  (let [bone-tx-data (create-bone parent-id bone)
        bone-id (tx-first-created bone-tx-data)
        child-bones (map (fn [index] (get bones index)) (.-children bone))
        children-tx-data (mapcat (fn [child] (create-bone-hierarchy bone-id bones child)) child-bones)]
    (concat bone-tx-data children-tx-data)))

(set! *warn-on-reflection* true)

(set! *warn-on-reflection* true)

(defn- create-bones [parent-id bones]
  (let [root-bones (filter is-root-bone? bones)
        tx-data (mapcat (fn [bone] (create-bone-hierarchy parent-id bones bone)) root-bones)]
    tx-data))

;; (defn- update-transforms [^Matrix4d parent bone]
;;   (let [t ^Matrix4d (:local-transform bone)
;;         t (doto (Matrix4d.)
;;             (.mul parent t))]
;;     (-> bone
;;         (assoc :transform t)
;;         (assoc :children (mapv #(update-transforms t %) (:children bone))))))

(g/defnk produce-transform [position rotation scale]
  (math/->mat4-non-uniform (Vector3d. (double-array position))
                           (math/euler-z->quat rotation)
                           (Vector3d. (double-array scale))))

;;//////////////////////////////////////////////////////////////////////////////////////////////

(defn- resource->bytes [resource]
  (with-open [in (io/input-stream resource)]
    (IOUtils/toByteArray in)))

(defn- handle-read-error [^Throwable error node-id resource]
  (let [^Throwable error (if (instance? java.lang.reflect.InvocationTargetException error) (.getCause error) error)
        msg (.getMessage error)
        path (resource/resource->proj-path resource)
        msg-missing-image "Region not found"]
    (cond
      (and (instance? spine-plugin-exception-cls error) (.contains msg msg-missing-image))
      (g/->error node-id :atlas :fatal resource (format "Atlas is missing image: %s" (subs msg (+ (count msg-missing-image) (str/index-of msg msg-missing-image) ))))

      (instance? spine-plugin-exception-cls error)
      (g/->error node-id :resource :fatal resource (format "Failed reading %s: %s" path msg))

      :else
      (g/->error node-id :resource :fatal resource (format "Couldn't read %s file %s: %s" spine-json-ext path msg)))))

; Loads the .spinejson file
(defn- load-spine-json
  ([node-id resource]
   (load-spine-json nil node-id resource))
  ([project node-id resource]
   (try
     (let [content (resource->bytes resource)
           path (resource/resource->proj-path resource)
           spine-data-handle (plugin-load-file-from-buffer content path) ; it throws if it fails to load
           animations (sort (vec (plugin-get-animations spine-data-handle)))
           bones (plugin-get-bones spine-data-handle)
           skins (sort (vec (plugin-get-skins spine-data-handle)))

           tx-data (concat
                    (g/set-property node-id :content content)
                    (g/set-property node-id :animations animations)
                    (g/set-property node-id :skins skins)
                    (g/set-property node-id :bones bones))

           all-tx-data (concat tx-data (create-bones node-id bones))]
       all-tx-data)
     (catch Exception error
       (handle-read-error error node-id resource)))))

(defn- build-spine-json [resource dep-resources user-data]
  {:resource resource :content (resource->bytes (:resource resource))})

(g/defnk produce-spine-json-build-targets [_node-id resource]
  (try
    [(bt/with-content-hash
       {:node-id _node-id
        :resource (workspace/make-build-resource resource)
        :build-fn build-spine-json
        :user-data {:content-hash (resource/resource->sha1-hex resource)}})]
    (catch Exception error
      (handle-read-error error _node-id resource))))

(g/defnode SpineSceneJson
  (inherits resource-node/ResourceNode)

  (property content g/Any)
  (property animations g/Any)
  (property skins g/Any)
  (property bones g/Any)

  (input child-bones g/Any :array)

  (output build-targets g/Any :cached produce-spine-json-build-targets))

;;//////////////////////////////////////////////////////////////////////////////////////////////

(g/defnk load-spine-data-handle [_node-id spine-json-resource spine-json-content atlas-resource texture-set-pb default-animation skin]
  ; The paths are used for error reporting if any loading goes wrong
  (try
    (when texture-set-pb
      (let [spine-json-path (resource/resource->proj-path spine-json-resource)
            atlas-path (resource/resource->proj-path atlas-resource)
            spine-data-handle (plugin-load-file-from-buffer spine-json-content spine-json-path texture-set-pb atlas-path) ; it throws if it fails to load
            _ (if (not (str/blank? default-animation)) (plugin-set-animation spine-data-handle default-animation))
            _ (if (not (str/blank? skin)) (plugin-set-skin spine-data-handle skin))
            _ (plugin-update-vertices spine-data-handle 0.0)]
        spine-data-handle))
    (catch Exception error
      (handle-read-error error _node-id spine-json-resource))))

(defn- sanitize-spine-scene [spine-scene-desc]
  {:pre (map? spine-scene-desc)} ; Spine$SpineSceneDesc in map format.
  (dissoc spine-scene-desc :sample-rate)) ; Deprecated field.

(defn- load-spine-scene [project self resource spine]
  (let [spine-resource (workspace/resolve-resource resource (:spine-json spine))
        atlas          (workspace/resolve-resource resource (:atlas spine))
        ; used for previewing a .spinescene as it doesn't have a material specified
        material       (workspace/resolve-resource resource spine-material-path)]
    (concat
     (g/connect project :default-tex-params self :default-tex-params)
     (g/set-property self
                     :spine-json spine-resource
                     :atlas atlas
                     :material material))))

;; (defn- make-spine-skeleton-scene [_node-id aabb gpu-texture scene-structure]
;;   (let [scene {:node-id _node-id :aabb aabb}]
;;     (if (and gpu-texture scene-structure)
;;       (let [blend-mode :blend-mode-alpha]
;;         {:aabb aabb
;;          :node-id _node-id
;;          :renderable {:render-fn render-spine-skeletons
;;                       :tags #{:spine :skeleton :outline}
;;                       :batch-key gpu-texture
;;                       :user-data {:scene-structure scene-structure}
;;                       :passes [pass/transparent]}})
;;       scene)))

(g/defnk produce-spine-scene [_node-id aabb main-scene gpu-texture aabb spine-data-handle]
  (if (some? main-scene)
    (assoc main-scene :children [;(make-spine-skeleton-scene _node-id aabb gpu-texture scene-structure)
                                 (make-spine-outline-scene _node-id aabb)])
    {:node-id _node-id :aabb aabb}))

(g/defnk produce-spine-scene-save-value [spine-json-resource atlas-resource]
  {:spine-json (resource/resource->proj-path spine-json-resource)
   :atlas (resource/resource->proj-path atlas-resource)})


(g/defnk produce-spine-scene-own-build-errors [_node-id atlas spine-json]
  (g/package-errors _node-id
                    (validate-scene-atlas _node-id atlas)
                    (validate-scene-spine-json _node-id spine-json)))

(defn- build-spine-scene [resource dep-resources user-data]
  (let [pb (:proto-msg user-data)
        pb (reduce #(assoc %1 (first %2) (second %2)) pb (map (fn [[label res]] [label (resource/proj-path (get dep-resources res))]) (:dep-resources user-data)))]
    {:resource resource :content (protobuf/map->bytes spine-plugin-spinescene-cls pb)}))


(g/defnk produce-spine-scene-build-targets
  [_node-id own-build-errors resource spine-json-resource atlas-resource spine-scene-pb dep-build-targets]
  (g/precluding-errors own-build-errors
                       (let [dep-build-targets (flatten dep-build-targets)
                             deps-by-source (into {} (map #(let [res (:resource %)] [(:resource res) res]) dep-build-targets))
                             dep-resources (map (fn [[label resource]] [label (get deps-by-source resource)]) [[:spine-json spine-json-resource] [:atlas atlas-resource]])]

                         [(bt/with-content-hash
                            {:node-id _node-id
                             :resource (workspace/make-build-resource resource)
                             :build-fn build-spine-scene
                             :user-data {:proto-msg spine-scene-pb
                                         :dep-resources dep-resources}

                             :deps dep-build-targets})])))

(g/defnode SpineSceneNode
  (inherits resource-node/ResourceNode)

  (property spine-json resource/Resource
            (value (gu/passthrough spine-json-resource))
            (set (fn [evaluation-context self old-value new-value]
                   (project/resource-setter evaluation-context self old-value new-value
                                            [:resource :spine-json-resource] ; Is this redundant?
                                            [:content :spine-json-content]
                                            [:animations :animations]
                                            [:skins :skins]
                                            [:bones :bones]
                                            [:node-outline :source-outline]
                                            [:build-targets :dep-build-targets])))
            (dynamic edit-type (g/constantly {:type resource/Resource :ext spine-json-ext}))
            (dynamic error (g/fnk [_node-id spine-json]
                                  (validate-scene-spine-json _node-id spine-json))))

  (property atlas resource/Resource
            (value (gu/passthrough atlas-resource))
            (set (fn [evaluation-context self old-value new-value]
                   (project/resource-setter evaluation-context self old-value new-value
                                            [:resource :atlas-resource]  ; Is this redundant?
                                            [:texture-set-pb :texture-set-pb]
                                            [:gpu-texture :gpu-texture]
                                            [:build-targets :dep-build-targets])))
            (dynamic edit-type (g/constantly {:type resource/Resource :ext "atlas"}))
            (dynamic error (g/fnk [_node-id atlas]
                                  (validate-scene-atlas _node-id atlas))))

  ; This property isn't visible, but here to allow us to preview the .spinescene
  (property material resource/Resource
            (value (gu/passthrough material-resource))
            (set (fn [evaluation-context self old-value new-value]
                   (project/resource-setter evaluation-context self old-value new-value
                                            [:shader :material-shader]
                                            [:samplers :material-samplers])))
            (dynamic edit-type (g/constantly {:type resource/Resource :ext "material"}))
            (dynamic visible (g/constantly false)))

  (input spine-json-resource resource/Resource)
  (input atlas-resource resource/Resource)

  (input material-shader ShaderLifecycle)
  (input material-samplers g/Any)
  (input material-resource resource/Resource) ; Just for being able to preview the asset

  (input default-tex-params g/Any)
  (input gpu-texture g/Any)
  (input dep-build-targets g/Any :array)

  (input animations g/Any)
  (output animations g/Any :cached (gu/passthrough animations))

  (input skins g/Any)
  (output skins g/Any :cached (gu/passthrough skins))

  (input bones g/Any)
  (output bones g/Any :cached (gu/passthrough bones))

  (output skin g/Str (g/fnk [skins] (first skins)))
  (output default-animation g/Str (g/fnk [animations] (first animations)))

  (output spine-data-handle g/Any :cached load-spine-data-handle) ; The c++ pointer
  (output aabb AABB :cached (g/fnk [spine-data-handle] (if (not (nil? spine-data-handle))
                                                         (get-aabb spine-data-handle)
                                                         geom/empty-bounding-box)))

  (input texture-set-pb g/Any)
  (output texture-set-pb g/Any :cached (gu/passthrough texture-set-pb))

  (input spine-json-content g/Any)
  (output spine-json-content g/Any :cached (gu/passthrough spine-json-content))

  (output save-value g/Any produce-spine-scene-save-value)
  (output own-build-errors g/Any produce-spine-scene-own-build-errors)
  (output build-targets g/Any :cached produce-spine-scene-build-targets)
  (output spine-scene-pb g/Any :cached produce-spine-scene-pb)
  (output main-scene g/Any :cached produce-main-scene)
  (output scene g/Any :cached produce-spine-scene))

;;//////////////////////////////////////////////////////////////////////////////////////////////

(g/defnk produce-model-pb [spine-scene-resource default-animation skin material-resource blend-mode create-go-bones playback-rate offset]
  (cond-> {:spine-scene (resource/resource->proj-path spine-scene-resource)
           :default-animation default-animation
           :skin skin
           :material (resource/resource->proj-path material-resource)
           :blend-mode blend-mode
           :create-go-bones create-go-bones}

           (not= 1.0 playback-rate)
           (assoc :playback-rate playback-rate)

           (not= 0.0 offset)
           (assoc :offset offset)))

(defn ->skin-choicebox [spine-skins]
  (properties/->choicebox (cons "" (remove (partial = "default") spine-skins))))

(defn validate-skin [node-id prop-kw spine-skins spine-skin]
  (when-not (empty? spine-skin)
    (validation/prop-error :fatal node-id prop-kw
                           (fn [skin skins]
                             (when-not (contains? skins skin)
                               (format "skin '%s' could not be found in the specified spine scene" skin)))
                           spine-skin
                           (disj (set spine-skins) "default"))))

(defn- validate-model-default-animation [node-id spine-scene animations default-animation]
  (when (and spine-scene (not-empty default-animation))
    (validation/prop-error :fatal node-id :default-animation
                           (fn [anim ids]
                             (when-not (contains? ids anim)
                               (format "animation '%s' could not be found in the specified spine scene" anim)))
                           default-animation
                           (set animations))))

(defn- validate-model-material [node-id material]
  (prop-resource-error :fatal node-id :material material "Material"))

(defn- validate-model-skin [node-id spine-scene skins skin]
  (when spine-scene
    (validate-skin node-id :skin skins skin)))

(defn- validate-model-spine-scene [node-id spine-scene]
  (prop-resource-error :fatal node-id :spine-scene spine-scene "Spine Scene"))

(g/defnk produce-model-own-build-errors [_node-id default-animation material animations spine-scene skins skin]
  (g/package-errors _node-id
                    (validate-model-material _node-id material)
                    (validate-model-spine-scene _node-id spine-scene)
                    (validate-model-skin _node-id spine-scene skins skin)
                    (validate-model-default-animation _node-id spine-scene animations default-animation)))

(defn- build-spine-model [resource dep-resources user-data]
  (let [pb (:proto-msg user-data)
        pb (reduce #(assoc %1 (first %2) (second %2)) pb (map (fn [[label res]] [label (resource/proj-path (get dep-resources res))]) (:dep-resources user-data)))]
    {:resource resource :content (protobuf/map->bytes (workspace/load-class! "com.dynamo.spine.proto.Spine$SpineModelDesc") pb)}))

(g/defnk produce-model-build-targets [_node-id own-build-errors resource model-pb spine-scene-resource material-resource dep-build-targets]
  (g/precluding-errors own-build-errors
                       (let [dep-build-targets (flatten dep-build-targets)
                             deps-by-source (into {} (map #(let [res (:resource %)] [(:resource res) res]) dep-build-targets))
                             dep-resources (map (fn [[label resource]] [label (get deps-by-source resource)]) [[:spine-scene spine-scene-resource] [:material material-resource]])
                             model-pb (update model-pb :skin (fn [skin] (or skin "")))]
                         [(bt/with-content-hash
                            {:node-id _node-id
                             :resource (workspace/make-build-resource resource)
                             :build-fn build-spine-model
                             :user-data {:proto-msg model-pb
                                         :dep-resources dep-resources}
                             :deps dep-build-targets})])))

(defn load-spine-model [project self resource spine]
  (let [resolve-fn (partial workspace/resolve-resource resource)
        spine (-> spine
                  (update :spine-scene resolve-fn)
                  (update :material resolve-fn))]
    (concat
     (g/connect project :default-tex-params self :default-tex-params)
     (for [[k v] spine]
       (g/set-property self k v)))))

(defn- step-animation
  [state dt spine-data-handle animation skin]
  (when (not (nil? spine-data-handle))
    (plugin-set-skin spine-data-handle skin)
    (plugin-set-animation spine-data-handle animation)
    (plugin-update-vertices spine-data-handle dt))
  state)

(g/defnk produce-spine-data-handle-updatable [_node-id spine-data-handle default-animation skin]
  (when (and (not (nil? spine-data-handle)) default-animation skin)
    {:node-id       _node-id
     :name          "Spine Scene Updater"
     :update-fn     (fn [state {:keys [dt]}] (step-animation state dt spine-data-handle default-animation skin))
     :initial-state {}}))

(g/defnode SpineModelNode
  (inherits resource-node/ResourceNode)

  (property spine-scene resource/Resource
            (value (gu/passthrough spine-scene-resource))
            (set (fn [evaluation-context self old-value new-value]
                   (project/resource-setter evaluation-context self old-value new-value
                                            [:resource :spine-scene-resource]
                                            [:spine-json-resource :spine-json-resource]
                                            [:atlas-resource :atlas-resource]
                                            [:main-scene :spine-main-scene]
                                            [:texture-set-pb :texture-set-pb]
                                            [:spine-json-content :spine-json-content]
                                            [:animations :animations]
                                            [:skins :skins]
                                            [:bones :bones]
                                            [:build-targets :dep-build-targets])))

            (dynamic edit-type (g/constantly {:type resource/Resource :ext spine-scene-ext}))
            (dynamic error (g/fnk [_node-id spine-scene]
                                  (validate-model-spine-scene _node-id spine-scene))))
  (property blend-mode g/Any (default :blend-mode-alpha)
            (dynamic tip (validation/blend-mode-tip blend-mode spine-plugin-blendmode-cls))
            (dynamic edit-type (g/constantly (properties/->pb-choicebox spine-plugin-blendmode-cls))))
  (property material resource/Resource
            (value (gu/passthrough material-resource))
            (set (fn [evaluation-context self old-value new-value]
                   (project/resource-setter evaluation-context self old-value new-value
                                            [:resource :material-resource]
                                            [:shader :material-shader]
                                            [:samplers :material-samplers]
                                            [:build-targets :dep-build-targets])))
            (dynamic edit-type (g/constantly {:type resource/Resource :ext "material"}))
            (dynamic error (g/fnk [_node-id material]
                                  (validate-model-material _node-id material))))
  (property default-animation g/Str
            (dynamic error (g/fnk [_node-id animations default-animation spine-scene]
                                  (validate-model-default-animation _node-id spine-scene animations default-animation)))
            (dynamic edit-type (g/fnk [animations] (properties/->choicebox animations))))
  (property skin g/Str
            (dynamic error (g/fnk [_node-id skin skins spine-scene]
                                  (validate-model-skin _node-id spine-scene skins skin)))
            (dynamic edit-type (g/fnk [skins] (->skin-choicebox skins))))
  (property create-go-bones g/Bool (default false))
  (property playback-rate g/Num (default 1.0))
  (property offset g/Num (default 0.0)
    (dynamic edit-type (g/constantly {:type :slider
                                      :min 0.0
                                      :max 1.0
                                      :precision 0.01})))

  (input spine-json-resource resource/Resource)
  (input atlas-resource resource/Resource)

  (input dep-build-targets g/Any :array)
  (input spine-scene-resource resource/Resource)
  (input spine-main-scene g/Any)
  (input material-resource resource/Resource)
  (input material-samplers g/Any)
  (input default-tex-params g/Any)

  (input animations g/Any)
  (input skins g/Any)
  (input bones g/Any)
  (input texture-set-pb g/Any)
  (input spine-json-content g/Any)

  (output spine-data-handle g/Any :cached load-spine-data-handle) ; The c++ pointer
  (output aabb AABB :cached (g/fnk [spine-data-handle] (if (not (nil? spine-data-handle))
                                                         (get-aabb spine-data-handle)
                                                         geom/empty-bounding-box)))

  (output tex-params g/Any (g/fnk [material-samplers default-tex-params]
                                  (or (some-> material-samplers first material/sampler->tex-params)
                                      default-tex-params)))

  (input material-shader ShaderLifecycle)
  (output material-shader ShaderLifecycle (gu/passthrough material-shader))

  (output updatable g/Any :cached produce-spine-data-handle-updatable)

  (output scene g/Any :cached (g/fnk [_node-id spine-main-scene aabb material-shader tex-params spine-data-handle default-animation skin updatable]
                                     (if (and (some? material-shader) (some? (:renderable spine-main-scene)))
                                       (let [aabb aabb
                                             spine-scene-node-id (:node-id spine-main-scene)]
                                         (-> spine-main-scene
                                             (assoc-in [:renderable :user-data :shader] material-shader)
                                             (update-in [:renderable :user-data :gpu-texture] texture/set-params tex-params)
                                             (assoc-in [:renderable :user-data :skin] skin)
                                             (assoc-in [:renderable :user-data :animation] default-animation)
                                             (assoc-in [:renderable :user-data :spine-data-handle] spine-data-handle)

                                             (assoc :updatable updatable)
                                             (assoc :aabb aabb)
                                             (assoc :children [(make-spine-outline-scene spine-scene-node-id aabb)])))
                                       (merge {:node-id _node-id
                                               :renderable {:passes [pass/selection]}
                                               :aabb geom/empty-bounding-box}
                                              spine-main-scene))))
  (output node-outline outline/OutlineData :cached (g/fnk [_node-id own-build-errors spine-scene]
                                                          (cond-> {:node-id _node-id
                                                                   :node-outline-key "Spine Model"
                                                                   :label "Spine Model"
                                                                   :icon spine-model-icon
                                                                   :outline-error? (g/error-fatal? own-build-errors)}

                                                            (resource/openable-resource? spine-scene)
                                                            (assoc :link spine-scene :outline-reference? false))))
  (output model-pb g/Any produce-model-pb)
  (output save-value g/Any (gu/passthrough model-pb))
  (output own-build-errors g/Any produce-model-own-build-errors)
  (output build-targets g/Any :cached produce-model-build-targets))

;;//////////////////////////////////////////////////////////////////////////////////////////////


(defn register-resource-types [workspace]
  (concat
   (resource-node/register-ddf-resource-type workspace
                                             :ext spine-scene-ext
                                             :label "Spine Scene"
                                             :node-type SpineSceneNode
                                             :ddf-type spine-plugin-spinescene-cls
                                             :sanitize-fn sanitize-spine-scene
                                             :load-fn load-spine-scene
                                             :icon spine-scene-icon
                                             :view-types [:scene :text]
                                             :view-opts {:scene {:grid true}}
                                             :template "/defold-spine/editor/resources/templates/template.spinescene")
   (resource-node/register-ddf-resource-type workspace
                                             :ext spine-model-ext
                                             :label "Spine Model"
                                             :node-type SpineModelNode
                                             :ddf-type spine-plugin-spinemodel-cls
                                             :load-fn load-spine-model
                                             :icon spine-model-icon
                                             :view-types [:scene :text]
                                             :view-opts {:scene {:grid true}}
                                             :tags #{:component}
                                             :tag-opts {:component {:transform-properties #{:position :rotation :scale}}}
                                             :template "/defold-spine/editor/resources/templates/template.spinemodel")
   (workspace/register-resource-type workspace
                                     :ext spine-json-ext
                                     :node-type SpineSceneJson
                                     :load-fn load-spine-json
                                     :icon spine-json-icon
                                     :view-types [:default]
                                     :tags #{:embeddable})))


; The plugin
(defn load-plugin-spine [workspace]
  (g/transact (concat (register-resource-types workspace))))

(defn return-plugin []
  (fn [x] (load-plugin-spine x)))
(return-plugin)
