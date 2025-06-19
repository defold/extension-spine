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

(ns editor.spineguiext
  (:require [clojure.string :as str]
            [dynamo.graph :as g]
            [editor.attachment :as attachment]
            [editor.core :as core]
            [editor.defold-project :as project]
            [editor.editor-extensions.graph :as ext-graph]
            [editor.editor-extensions.node-types :as node-types]
            [editor.geom :as geom]
            [editor.gl.texture]
            [editor.graph-util :as gu]
            [editor.gui :as gui]
            [editor.outline :as outline]
            [editor.properties :as properties]
            [editor.protobuf :as protobuf]
            [editor.resource :as resource]
            [editor.spineext :as spineext]
            [editor.util :as util]
            [editor.workspace :as workspace]
            [internal.graph.types :as gt]
            [util.coll :as coll :refer [pair]]
            [util.murmur :as murmur])
  (:import [com.dynamo.gamesys.proto Gui$NodeDesc Gui$NodeDesc$ClippingMode]
           [editor.gl.texture TextureLifecycle]
           [javax.vecmath Matrix4d Point3d]))

(set! *warn-on-reflection* true)

; Plugin functions (from Spine.java)

;; (defn- debug-cls [^Class cls]
;;   (doseq [m (.getMethods cls)]
;;     (prn (.toString m))
;;     (println "Method Name: " (.getName m) "(" (.getParameterTypes m) ")")
;;     (println "Return Type: " (.getReturnType m) "\n")))

; More about JNA + Clojure
; https://nakkaya.com/2009/11/16/java-native-access-from-clojure/


;;//////////////////////////////////////////////////////////////////////////////////////////////

;; Spine nodes

(def ^:private validate-spine-scene (partial gui/validate-required-gui-resource "spine scene '%s' does not exist in the scene" :spine-scene))

(defn- validate-spine-default-animation [node-id spine-scene-names spine-anim-ids spine-default-animation spine-scene]
  (when-not (g/error? (validate-spine-scene node-id spine-scene-names spine-scene))
    (gui/validate-optional-gui-resource "animation '%s' could not be found in the specified spine scene" :spine-default-animation node-id spine-anim-ids spine-default-animation)))

(defn- validate-spine-skin [node-id spine-scene-names spine-skin-ids spine-skin spine-scene]
  (when-not (g/error? (validate-spine-scene node-id spine-scene-names spine-scene))
    (spineext/validate-skin node-id :spine-skin spine-skin-ids spine-skin)))

(defn- transform-vtx [^Matrix4d m4d color vtx]
  (let [[cr cg cb ca] color
        [x y z u v r g b a page_index] vtx
        p (Point3d.)
        _ (.set p x y z)
        _ (.transform m4d p)]
    [(.x p) (.y p) (.z p) u v (* r cr) (* g cg) (* b cb) (* a ca) page_index]))

(defn- produce-local-vertices [handle skin anim dt]
  (if (not= handle nil)
    (let [_ (if (not (str/blank? skin)) (spineext/plugin-set-skin handle skin))
          _ (if (not (str/blank? anim)) (spineext/plugin-set-animation handle anim))
          _ (spineext/plugin-update-vertices handle dt)
          vb-data (spineext/plugin-get-vertex-buffer-data handle) ; list of SpineVertex
          vb-data-vec (spineext/transform-vertices-as-vec vb-data)] ; unpacked into lists of lists [[x y z u v r g b a page_index]])
      vb-data-vec)
    []))

(defn- renderable->vertices [user-data renderable]
  (let [handle (spineext/renderable->handle renderable)
        world-transform (:world-transform renderable)
        vertex-buffer (:spine-vertex-buffer user-data)
        color (:color user-data)
        vb-data-transformed (map (fn [vtx] (transform-vtx world-transform color vtx)) vertex-buffer)]
    vb-data-transformed))

(defn- gen-vb [user-data renderables]
  (let [vertices (mapcat (fn [renderable] (renderable->vertices user-data renderable)) renderables)
        vb-out (spineext/generate-vertex-buffer vertices)]
    vb-out))

(g/defnk produce-spine-node-msg [visual-base-node-msg ^:raw spine-scene ^:raw spine-default-animation ^:raw spine-skin ^:raw clipping-mode ^:raw clipping-visible ^:raw clipping-inverted]
  (merge visual-base-node-msg
         (protobuf/make-map-without-defaults Gui$NodeDesc
           :size-mode :size-mode-auto
           :spine-scene spine-scene
           :spine-default-animation spine-default-animation
           :spine-skin spine-skin
           :clipping-mode clipping-mode
           :clipping-visible clipping-visible
           :clipping-inverted clipping-inverted)))

(g/defnode SpineNode
  (inherits gui/VisualNode)

  (property spine-scene g/Str (default (protobuf/default Gui$NodeDesc :spine-scene))
            (dynamic edit-type (g/fnk [basic-gui-scene-info]
                                 (let [spine-scene-names (:spine-scene-names basic-gui-scene-info)]
                                   (gui/wrap-layout-property-edit-type spine-scene (gui/required-gui-resource-choicebox spine-scene-names)))))
            (dynamic error (g/fnk [_node-id basic-gui-scene-info spine-scene]
                             (let [spine-scene-names (:spine-scene-names basic-gui-scene-info)]
                               (validate-spine-scene _node-id spine-scene-names spine-scene))))
            (value (gui/layout-property-getter spine-scene))
            (set (gui/layout-property-setter spine-scene)))
  (property spine-default-animation g/Str (default (protobuf/default Gui$NodeDesc :spine-default-animation))
            (dynamic label (g/constantly "Default Animation"))
            (dynamic error (g/fnk [_node-id basic-gui-scene-info spine-anim-ids spine-default-animation spine-scene]
                             (let [spine-scene-names (:spine-scene-names basic-gui-scene-info)]
                               (validate-spine-default-animation _node-id spine-scene-names spine-anim-ids spine-default-animation spine-scene))))
            (dynamic edit-type (g/fnk [spine-anim-ids] (gui/wrap-layout-property-edit-type spine-default-animation (gui/optional-gui-resource-choicebox spine-anim-ids))))
            (value (gui/layout-property-getter spine-default-animation))
            (set (gui/layout-property-setter spine-default-animation)))
  (property spine-skin g/Str (default (protobuf/default Gui$NodeDesc :spine-skin))
            (dynamic label (g/constantly "Skin"))
            (dynamic error (g/fnk [_node-id basic-gui-scene-info spine-scene spine-skin spine-skin-ids]
                             (let [spine-scene-names (:spine-scene-names basic-gui-scene-info)]
                               (validate-spine-skin _node-id spine-scene-names spine-skin-ids spine-skin spine-scene))))
            (dynamic edit-type (g/fnk [spine-skin-ids] (gui/wrap-layout-property-edit-type spine-skin (spineext/->skin-choicebox spine-skin-ids))))
            (value (gui/layout-property-getter spine-skin))
            (set (gui/layout-property-setter spine-skin)))
  (property clipping-mode g/Keyword (default (protobuf/default Gui$NodeDesc :clipping-mode))
            (dynamic edit-type (gui/layout-property-edit-type clipping-mode (properties/->pb-choicebox Gui$NodeDesc$ClippingMode)))
            (value (gui/layout-property-getter clipping-mode))
            (set (gui/layout-property-setter clipping-mode)))
  (property clipping-visible g/Bool (default (protobuf/default Gui$NodeDesc :clipping-visible))
            (dynamic edit-type (gui/layout-property-edit-type clipping-visible {:type g/Bool}))
            (value (gui/layout-property-getter clipping-visible))
            (set (gui/layout-property-setter clipping-visible)))
  (property clipping-inverted g/Bool (default (protobuf/default Gui$NodeDesc :clipping-inverted))
            (dynamic edit-type (gui/layout-property-edit-type clipping-inverted {:type g/Bool}))
            (value (gui/layout-property-getter clipping-inverted))
            (set (gui/layout-property-setter clipping-inverted)))

  (display-order (into gui/base-display-order
                       [:spine-scene :spine-default-animation :spine-skin :color :alpha :inherit-alpha :layer :blend-mode :pivot :x-anchor :y-anchor
                        :adjust-mode :clipping :visible-clipper :inverted-clipper]))

  (output node-msg g/Any :cached produce-spine-node-msg)
  (output spine-anim-ids gui/GuiResourceNames
          (g/fnk [basic-gui-scene-info spine-scene]
            (let [spine-scene-element-ids (:spine-scene-element-ids basic-gui-scene-info)]
              (:spine-anim-ids (or (get spine-scene-element-ids spine-scene)
                                   (get spine-scene-element-ids ""))))))
  (output spine-skin-ids gui/GuiResourceNames
          (g/fnk [basic-gui-scene-info spine-scene]
            (let [spine-scene-element-ids (:spine-scene-element-ids basic-gui-scene-info)]
              (:spine-skin-ids (or (get spine-scene-element-ids spine-scene)
                                   (get spine-scene-element-ids ""))))))
  (output spine-scene-scene g/Any
          (g/fnk [costly-gui-scene-info spine-scene]
            (let [spine-scene-infos (:spine-scene-infos costly-gui-scene-info)]
              (:spine-scene-scene (or (get spine-scene-infos spine-scene)
                                      (get spine-scene-infos ""))))))
  (output spine-scene-bones g/Any
          (g/fnk [costly-gui-scene-info spine-scene]
            (let [spine-scene-infos (:spine-scene-infos costly-gui-scene-info)]
              (:spine-bones (or (get spine-scene-infos spine-scene)
                                (get spine-scene-infos ""))))))
  (output spine-scene-pb g/Any
          (g/fnk [costly-gui-scene-info spine-scene]
            (let [spine-scene-infos (:spine-scene-infos costly-gui-scene-info)]
              (:spine-scene-pb (or (get spine-scene-infos spine-scene)
                                   (get spine-scene-infos ""))))))

  ;; The handle to the C++ resource
  (output spine-data-handle g/Any
          (g/fnk [costly-gui-scene-info spine-scene]
            (let [spine-scene-infos (:spine-scene-infos costly-gui-scene-info)]
              (:spine-data-handle (or (get spine-scene-infos spine-scene)
                                      (get spine-scene-infos ""))))))

  (output spine-vertex-buffer g/Any :cached (g/fnk [spine-scene spine-data-handle spine-skin spine-default-animation]
                                                   (produce-local-vertices spine-data-handle spine-skin spine-default-animation 0.0)))

  (output aabb g/Any
          (g/fnk [costly-gui-scene-info spine-scene spine-skin]
            (let [spine-scene-infos (:spine-scene-infos costly-gui-scene-info)
                  spine-skin-name (if (= spine-skin "") "default" spine-skin)]
              (or (get-in spine-scene-infos [spine-scene :spine-skin-aabbs spine-skin-name])
                  geom/empty-bounding-box))))

  ; Overloaded outputs from VisualNode
  (output gpu-texture TextureLifecycle (g/constantly nil))
  (output scene-renderable-user-data g/Any :cached (g/fnk [spine-scene-scene spine-vertex-buffer color+alpha clipping-mode clipping-inverted clipping-visible]
                                                          (let [user-data (assoc (get-in spine-scene-scene [:renderable :user-data])
                                                                                 :color color+alpha
                                                                                 :renderable-tags #{:gui-spine}
                                                                                 :gen-vb gen-vb
                                                                                 :spine-vertex-buffer spine-vertex-buffer)]
                                                            (cond-> user-data
                                                              (not= :clipping-mode-none clipping-mode)
                                                              (assoc :clipping {:mode clipping-mode :inverted clipping-inverted :visible clipping-visible})))))

  (output own-build-errors g/Any
          (g/fnk [_node-id basic-gui-scene-info build-errors-visual-node spine-anim-ids spine-default-animation spine-skin-ids spine-skin spine-scene]
            (let [spine-scene-names (:spine-scene-names basic-gui-scene-info)]
              (g/package-errors
                _node-id
                build-errors-visual-node
                (validate-spine-scene _node-id spine-scene-names spine-scene)
                (validate-spine-default-animation _node-id spine-scene-names spine-anim-ids spine-default-animation spine-scene)
                (validate-spine-skin _node-id spine-scene-names spine-skin-ids spine-skin spine-scene))))))

(defmethod gui/update-gui-resource-reference [::SpineNode :spine-scene]
  [_ evaluation-context node-id old-name new-name]
  (gui/update-basic-gui-resource-reference evaluation-context node-id :spine-scene old-name new-name))

;;//////////////////////////////////////////////////////////////////////////////////////////////

(g/defnk produce-spine-scene-element-ids [name spine-data-handle spine-anim-ids spine-skins spine-scene]
  ;; If the referenced spine-scene-resource is missing, we don't return an entry.
  ;; This will cause every usage to fall back on the no-spine-scene entry for "".
  ;; NOTE: the no-spine-scene entry uses an instance of SpineSceneNode with an empty name.
  ;; It does not have any data, but it should still return an entry.
  (when (or (and (= "" name) (nil? spine-scene))
            (every? some? [spine-data-handle spine-anim-ids spine-skins]))
    {name {:spine-anim-ids (into (sorted-set) spine-anim-ids)
           :spine-skin-ids (into (sorted-set) spine-skins)}}))

(g/defnk produce-spine-scene-infos [_node-id name spine-data-handle spine-scene spine-bones spine-scene-pb spine-scene-scene spine-skin-aabbs]
  ;; If the referenced spine-scene-resource is missing, we don't return an entry.
  ;; This will cause every usage to fall back on the no-spine-scene entry for "".
  ;; NOTE: the no-spine-scene entry uses an instance of SpineSceneNode with an empty name.
  ;; It does not have any data, but it should still return an entry.
  (when (or (and (= "" name) (nil? spine-scene))
            (every? some? [spine-data-handle spine-scene-pb spine-scene-scene]))
    {name {:spine-data-handle spine-data-handle
           :spine-bones spine-bones
           :spine-skin-aabbs spine-skin-aabbs
           :spine-scene-pb spine-scene-pb
           :spine-scene-scene spine-scene-scene}}))

(g/defnode SpineSceneNode
  (inherits outline/OutlineNode)
  (property name g/Str
            (dynamic error (g/fnk [_node-id name name-counts] (gui/prop-unique-id-error _node-id :name name name-counts "Name")))
            (set (partial gui/update-gui-resource-references :spine-scene)))
  (property spine-scene resource/Resource
            (value (gu/passthrough spine-scene-resource))
            (set (fn [evaluation-context self old-value new-value]
                   (project/resource-setter
                    evaluation-context self old-value new-value
                    [:resource :spine-scene-resource]
                    [:build-targets :dep-build-targets]
                    [:scene :spine-scene-scene]
                    [:skin-aabbs :spine-skin-aabbs]
                    [:spine-scene-pb :spine-scene-pb]
                    [:spine-data-handle :spine-data-handle]
                    [:animations :spine-anim-ids]
                    [:skins :spine-skins]
                    [:bones :spine-bones])))
            (dynamic error (g/fnk [_node-id spine-scene]
                                  (gui/prop-resource-error _node-id :spine-scene spine-scene "Spine Scene")))
            (dynamic edit-type (g/constantly
                                {:type resource/Resource
                                 :ext ["spinescene"]})))

  (input dep-build-targets g/Any)
  (input name-counts gui/NameCounts)
  (input spine-scene-resource resource/Resource)

  (input spine-data-handle g/Any :substitute nil) ; The c++ pointer
  (output spine-data-handle g/Any (gu/passthrough spine-data-handle))

  (input spine-anim-ids g/Any :substitute (constantly nil))
  (input spine-skins g/Any :substitute (constantly nil))
  (input spine-bones g/Any :substitute (constantly nil))

  (input spine-scene-scene g/Any :substitute (constantly nil))
  (input spine-skin-aabbs g/Any :substitute (constantly nil))
  (input spine-scene-pb g/Any :substitute (constantly nil))

  (output dep-build-targets g/Any (gu/passthrough dep-build-targets))
  (output node-outline outline/OutlineData :cached (g/fnk [_node-id name spine-scene-resource build-errors spine-skins spine-anim-ids]
                                                          (cond-> {:node-id _node-id
                                                                   :node-outline-key name
                                                                   :label name
                                                                   :icon spineext/spine-scene-icon
                                                                   :outline-error? (g/error-fatal? build-errors)}
                                                            (resource/openable-resource? spine-scene-resource) (assoc :link spine-scene-resource :outline-show-link? true))))
  (output pb-msg g/Any (g/fnk [name spine-scene]
                              {:name name
                               :path (resource/resource->proj-path spine-scene)}))
  (output spine-scene-element-ids gui/SpineSceneElementIds :cached produce-spine-scene-element-ids)
  (output spine-scene-infos gui/SpineSceneInfos :cached produce-spine-scene-infos)
  (output build-errors g/Any (g/fnk [_node-id name name-counts spine-scene]
                                    (g/package-errors _node-id
                                                      (gui/prop-unique-id-error _node-id :name name name-counts "Name")
                                                      (gui/prop-resource-error _node-id :spine-scene spine-scene "Spine Scene")))))

;;//////////////////////////////////////////////////////////////////////////////////////////////

(defn- attach-spine-scene
  ;; self is the GuiSceneNode
  [self spine-scenes-node spine-scene]
  (concat
    (g/connect spine-scene :_node-id spine-scenes-node :nodes)
    (g/connect spine-scene :spine-scene-element-ids self :spine-scene-element-ids)
    (g/connect spine-scene :spine-scene-infos self :spine-scene-infos)
    (g/connect spine-scene :name self :spine-scene-names)
    (g/connect spine-scene :dep-build-targets self :dep-build-targets)
    (g/connect spine-scene :pb-msg self :resource-msgs)
    (g/connect spine-scene :build-errors spine-scenes-node :build-errors)
    (g/connect spine-scene :node-outline spine-scenes-node :child-outlines)
    (g/connect spine-scene :name spine-scenes-node :names)
    (g/connect spine-scenes-node :name-counts spine-scene :name-counts)))

(defn- add-spine-scene [scene spine-scenes-node resource name]
  (g/make-nodes (g/node-id->graph-id scene) [node [SpineSceneNode :name name :spine-scene resource]]
                (attach-spine-scene scene spine-scenes-node node)))

(defn- add-spine-scenes-handler [project {:keys [scene parent]} select-fn]
  (gui/query-and-add-resources!
   "Spine Scenes" [spineext/spine-scene-ext] (g/node-value parent :name-counts) project select-fn
   (partial add-spine-scene scene parent)))

(g/defnode SpineScenesNode
  (inherits core/Scope)
  (inherits outline/OutlineNode)
  (input names g/Str :array)
  (output name-counts gui/NameCounts :cached (g/fnk [names] (frequencies names)))
  (input build-errors g/Any :array)
  (output build-errors g/Any (gu/passthrough build-errors))
  (output node-outline outline/OutlineData :cached
          (gui/gen-outline-fnk
            "Spine Scenes" "Spine Scenes" 6 false
            [{:node-type SpineSceneNode
              :tx-attach-fn (gui/gen-outline-node-tx-attach-fn attach-spine-scene)}]))
  (output add-handler-info g/Any
          (g/fnk [_node-id]
                 [_node-id "Spine Scenes..." spineext/spine-scene-icon add-spine-scenes-handler {}])))

;;//////////////////////////////////////////////////////////////////////////////////////////////


;; Loading a gui scene

(defn- load-gui-scene-spine [_project self scene graph-id resource]
  (g/make-nodes graph-id [spine-scenes-node SpineScenesNode]
    (g/connect spine-scenes-node :_node-id self :nodes)
    (g/connect spine-scenes-node :build-errors self :build-errors)
    (g/connect spine-scenes-node :node-outline self :child-outlines)
    (g/connect spine-scenes-node :add-handler-info self :handler-infos) 
    (let [spine-scene-name+paths
          (concat
            (->> (:spine-scenes scene)
                 (map (juxt :name :spine-scene)))
            (->> (:resources scene)
                 (keep (fn [{:keys [name path]}]
                         (when (str/ends-with? path spineext/spine-scene-ext)
                           (pair name path))))))]
      (for [[name path] spine-scene-name+paths]
        (let [spine-scene-resource (workspace/resolve-resource resource path)]
          (g/make-nodes graph-id [spine-scene [SpineSceneNode :name name :spine-scene spine-scene-resource]]
            (attach-spine-scene self spine-scenes-node spine-scene)))))))

;;//////////////////////////////////////////////////////////////////////////////////////////////

(defn- gui-scene-node->spine-scenes-node [basis gui-scene-node]
  (coll/some
    (fn [arc]
      (let [id (gt/source-id arc)]
        (when (= SpineScenesNode (g/node-type* basis id))
          id)))
    (g/explicit-arcs-by-target basis gui-scene-node :nodes)))

(defn- attach-spine-scene-to-gui-scene [{:keys [basis]} gui-scene-node spine-scene-node]
  (attach-spine-scene gui-scene-node (gui-scene-node->spine-scenes-node basis gui-scene-node) spine-scene-node))

(defmethod ext-graph/init-attachment ::SpineSceneNode [evaluation-context rt project parent-node-id _ child-node-id attachment]
  (-> attachment
      (util/provide-defaults
        "name" (ext-graph/gen-gui-component-name attachment "spine_scene" gui-scene-node->spine-scenes-node rt parent-node-id evaluation-context))
      (ext-graph/attachment->set-tx-steps child-node-id rt project evaluation-context)))

(defn- fixup-spine-node [node-type-info node-desc]
  (let [node-type (:type node-desc)]
    (cond-> (assoc node-desc
              :size-mode :size-mode-auto)

            (= :type-spine node-type)
            (assoc
              :type (:output-node-type node-type-info)
              :custom-type (:output-custom-type node-type-info)))))

(node-types/register-node-type-name! SpineNode "gui-node-type-spine")

(defn- register-gui-resource-types! [workspace]
  (gui/register-gui-scene-loader! load-gui-scene-spine)
  (let [info {:node-type :type-custom
              :node-cls SpineNode
              :display-name "Spine"
              :custom-type (murmur/hash32 "Spine")
              :icon spineext/spine-scene-icon
              :convert-fn fixup-spine-node
              :defaults gui/visual-base-node-defaults}
        info-depr (merge info {:node-type :type-spine
                               :custom-type 0
                               :output-node-type (:node-type info)
                               :output-custom-type (:custom-type info)
                               :deprecated true})]
    (gui/register-node-type-info! info)
    ; Register :type-spine with custom type 0 in order to be able to read old files
    (gui/register-node-type-info! info-depr))
  (g/transact
    (concat
      (gui/register-node-tree-attachment-node-type workspace SpineNode)
      (attachment/register
        workspace gui/GuiSceneNode :spine-scenes
        :add {SpineSceneNode (partial g/expand-ec attach-spine-scene-to-gui-scene)}
        :get (fn get-spine-scenes [gui-scene-node {:keys [basis] :as evaluation-context}]
               (attachment/nodes-getter (gui-scene-node->spine-scenes-node basis gui-scene-node) evaluation-context))))))

; The plugin
(defn load-plugin-spine-gui [workspace]
  (register-gui-resource-types! workspace))

(defn return-plugin []
  (fn [x] (load-plugin-spine-gui x)))
(return-plugin)
