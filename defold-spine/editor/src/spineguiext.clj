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
  (:require [schema.core :as s]
            [clojure.string :as str]
            [dynamo.graph :as g]
            [util.murmur :as murmur]
            [editor.graph-util :as gu]
            [editor.geom :as geom]
            [editor.defold-project :as project]
            [editor.resource :as resource]
            [editor.scene-cache :as scene-cache] ; debug only
            [editor.workspace :as workspace]
            [editor.types :as types]
            [editor.outline :as outline]
            [editor.properties :as properties]
            [editor.gui :as gui]
            [editor.spineext :as spineext])
  (:import [com.dynamo.gamesys.proto Gui$NodeDesc$ClippingMode]
           [editor.gl.texture TextureLifecycle]
           [javax.vecmath Matrix4d Point3d]))


(set! *warn-on-reflection* true)

(g/deftype ^:private SpineSceneElementIds s/Any #_{s/Str {:spine-anim-ids (sorted-set s/Str)
                                                          :spine-skin-ids (sorted-set s/Str)}})
(g/deftype ^:private SpineSceneInfos s/Any #_{s/Str {:spine-data-handle (s/maybe {s/Int s/Any})
                                                     :spine-bones (s/maybe {s/Keyword s/Any})
                                                     :spine-scene-scene (s/maybe {s/Keyword s/Any})
                                                     :spine-scene-pb (s/maybe {s/Keyword s/Any})}})

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

(defn- transform-vtx [^Matrix4d m4d vtx]
  (let [p (Point3d.)
        [x y z u v r g b a] vtx
        _ (.set p x y z)
        _ (.transform m4d p)]
    [(.x p) (.y p) (.z p) u v r g b a]))

(defn- produce-local-vertices [handle skin anim dt]
  (if (not= handle nil)
    (let [_ (if (not (str/blank? skin)) (spineext/plugin-set-skin handle skin))
          _ (if (not (str/blank? anim)) (spineext/plugin-set-animation handle anim))
          _ (spineext/plugin-update-vertices handle dt)
          vb-data (spineext/plugin-get-vertex-buffer-data handle) ; list of SpineVertex
          vb-data-vec (spineext/transform-vertices-as-vec vb-data)] ; unpacked into lists of lists [[x y z u v r g b a]])
      vb-data-vec)
    []))

(defn- renderable->vertices [user-data renderable]
  (let [handle (spineext/renderable->handle renderable)
        world-transform (:world-transform renderable)
        vertex-buffer (:spine-vertex-buffer user-data)
        vb-data-transformed (map (fn [vtx] (transform-vtx world-transform vtx)) vertex-buffer)]
    vb-data-transformed))

(defn- gen-vb [user-data renderables]
  (let [vertices (mapcat (fn [renderable] (renderable->vertices user-data renderable)) renderables)
        vb-out (spineext/generate-vertex-buffer vertices)]
    vb-out))

(g/defnk produce-spine-node-msg [visual-base-node-msg spine-scene spine-default-animation spine-skin clipping-mode clipping-visible clipping-inverted]
  (assoc visual-base-node-msg
    :size-mode :size-mode-auto
    :spine-scene spine-scene
    :spine-default-animation spine-default-animation
    :spine-skin spine-skin
    :clipping-mode clipping-mode
    :clipping-visible clipping-visible
    :clipping-inverted clipping-inverted))

(g/defnode SpineNode
  (inherits gui/VisualNode)

  (property spine-scene g/Str
            (default "")
            (dynamic edit-type (g/fnk [spine-scene-names] (gui/required-gui-resource-choicebox spine-scene-names)))
            (dynamic error (g/fnk [_node-id spine-scene spine-scene-names]
                                  (validate-spine-scene _node-id spine-scene-names spine-scene))))
  (property spine-default-animation g/Str
            (dynamic label (g/constantly "Default Animation"))
            (dynamic error (g/fnk [_node-id spine-anim-ids spine-default-animation spine-scene spine-scene-names]
                                  (validate-spine-default-animation _node-id spine-scene-names spine-anim-ids spine-default-animation spine-scene)))
            (dynamic edit-type (g/fnk [spine-anim-ids] (gui/optional-gui-resource-choicebox spine-anim-ids))))
  (property spine-skin g/Str
            (dynamic label (g/constantly "Skin"))
            (dynamic error (g/fnk [_node-id spine-scene spine-scene-names spine-skin spine-skin-ids]
                                  (validate-spine-skin _node-id spine-scene-names spine-skin-ids spine-skin spine-scene)))
            (dynamic edit-type (g/fnk [spine-skin-ids] (spineext/->skin-choicebox spine-skin-ids))))

  (property clipping-mode g/Keyword (default :clipping-mode-none)
            (dynamic edit-type (g/constantly (properties/->pb-choicebox Gui$NodeDesc$ClippingMode))))
  (property clipping-visible g/Bool (default true))
  (property clipping-inverted g/Bool (default false))

  (property size types/Vec3
            (dynamic visible (g/constantly false)))

  (display-order (into gui/base-display-order
                       [:spine-scene :spine-default-animation :spine-skin :color :alpha :inherit-alpha :layer :blend-mode :pivot :x-anchor :y-anchor
                        :adjust-mode :clipping :visible-clipper :inverted-clipper]))

  (output node-msg g/Any :cached produce-spine-node-msg)
  (output spine-anim-ids gui/GuiResourceNames (g/fnk [spine-scene-element-ids spine-scene gui-scene]
                                                     (:spine-anim-ids (or (spine-scene-element-ids spine-scene)
                                                                          (spine-scene-element-ids "")))))
  (output spine-skin-ids gui/GuiResourceNames (g/fnk [spine-scene-element-ids spine-scene]
                                                     (:spine-skin-ids (or (spine-scene-element-ids spine-scene)
                                                                          (spine-scene-element-ids "")))))
  (output spine-scene-scene g/Any (g/fnk [spine-scene-infos spine-scene]
                                         (:spine-scene-scene (or (spine-scene-infos spine-scene)
                                                                 (spine-scene-infos "")))))
  (output spine-scene-bones g/Any (g/fnk [spine-scene-infos spine-scene]
                                         (:spine-bones (or (spine-scene-infos spine-scene)
                                                           (spine-scene-infos "")))))

  (output spine-scene-pb g/Any (g/fnk [spine-scene-infos spine-scene]
                                      (:spine-scene-pb (or (spine-scene-infos spine-scene)
                                                           (spine-scene-infos "")))))
  
  ;; The handle to the C++ resource
  (output spine-data-handle g/Any (g/fnk [spine-scene-infos spine-scene]
                                         (:spine-data-handle (or (spine-scene-infos spine-scene)
                                                                 (spine-scene-infos "")))))
  
  (output spine-vertex-buffer g/Any :cached (g/fnk [spine-scene spine-data-handle spine-skin spine-default-animation]
                                                   (produce-local-vertices spine-data-handle spine-skin spine-default-animation 0.0)))
  
  (output aabb g/Any (g/fnk [spine-scene-infos spine-scene spine-skin pivot]
                            (or (get-in spine-scene-infos [spine-scene :spine-skin-aabbs (if (= spine-skin "") "default" spine-skin)])
                                geom/empty-bounding-box)))

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

  ;; (output bone-node-msgs g/Any :cached (g/fnk [node-msgs spine-scene-structure spine-scene-pb adjust-mode]
  ;;                                             (let [pb-msg (first node-msgs)
  ;;                                                   gui-node-id (:id pb-msg)
  ;;                                                   id-fn (fn [b] (format "%s/%s" gui-node-id (:name b)))
  ;;                                                   bones (tree-seq :children :children (:skeleton spine-scene-structure))
  ;;                                                   bone-order (zipmap (map id-fn (-> spine-scene-pb :skeleton :bones)) (range))
  ;;                                                   child-to-parent (reduce (fn [m b] (into m (map (fn [c] [(:name c) b]) (:children b)))) {} bones)
  ;;                                                   bone-msg {:spine-node-child true
  ;;                                                             :size [0.0 0.0 0.0 0.0]
  ;;                                                             :position [0.0 0.0 0.0 0.0]
  ;;                                                             :scale [1.0 1.0 1.0 0.0]
  ;;                                                             :type :type-box
  ;;                                                             :adjust-mode adjust-mode}
  ;;                                                   bone-msgs (mapv (fn [b] (assoc bone-msg :id (id-fn b) :parent (if (contains? child-to-parent (:name b))
  ;;                                                                                                                   (id-fn (get child-to-parent (:name b)))
  ;;                                                                                                                   gui-node-id))) bones)]
  ;;                                          ;; Bone nodes need to be sorted in same order as bones in rig scene
  ;;                                               (sort-by #(bone-order (:id %)) bone-msgs))))
  (output bone-node-msgs g/Any :cached [])
        
  (output node-rt-msgs g/Any :cached
          (g/fnk [node-msgs node-rt-msgs bone-node-msgs spine-skin-ids]
                 (let [pb-msg (first node-msgs)
                       rt-pb-msgs (into node-rt-msgs [(update pb-msg :spine-skin (fn [skin] (or skin "")))])]
                   (into rt-pb-msgs bone-node-msgs))))
  (output own-build-errors g/Any (g/fnk [_node-id build-errors-visual-node spine-anim-ids spine-default-animation spine-skin-ids spine-skin spine-scene spine-scene-names]
                                        (g/package-errors _node-id
                                                          build-errors-visual-node
                                                          (validate-spine-scene _node-id spine-scene-names spine-scene)
                                                          (validate-spine-default-animation _node-id spine-scene-names spine-anim-ids spine-default-animation spine-scene)
                                                          (validate-spine-skin _node-id spine-scene-names spine-skin-ids spine-skin spine-scene)))))


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
  (output spine-scene-element-ids SpineSceneElementIds :cached produce-spine-scene-element-ids)
  (output spine-scene-infos SpineSceneInfos :cached produce-spine-scene-infos)
  (output spine-scene-names gui/GuiResourceNames (g/fnk [name] (sorted-set name)))
  (output build-errors g/Any (g/fnk [_node-id name name-counts spine-scene]
                                    (g/package-errors _node-id
                                                      (gui/prop-unique-id-error _node-id :name name name-counts "Name")
                                                      (gui/prop-resource-error _node-id :spine-scene spine-scene "Spine Scene")))))

;;//////////////////////////////////////////////////////////////////////////////////////////////

(defn- attach-spine-scene
  ;; self is the GuiSceneNode
  ([self spine-scenes-node spine-scene]
   (attach-spine-scene self spine-scenes-node spine-scene false))
  ([self spine-scenes-node spine-scene internal?]
   (concat
    (g/connect spine-scene :_node-id self :nodes)
    (g/connect spine-scene :spine-scene-element-ids self :spine-scene-element-ids)
    (g/connect spine-scene :spine-scene-infos self :spine-scene-infos)
    (when (not internal?)
      (concat
       (g/connect spine-scene :spine-scene-names self :spine-scene-names)
       (g/connect spine-scene :dep-build-targets self :dep-build-targets)
       (g/connect spine-scene :pb-msg self :resource-msgs)
       (g/connect spine-scene :build-errors spine-scenes-node :build-errors)
       (g/connect spine-scene :node-outline spine-scenes-node :child-outlines)
       (g/connect spine-scene :name spine-scenes-node :names)
       (g/connect spine-scenes-node :name-counts spine-scene :name-counts))))))

(defn- add-spine-scene [scene spine-scenes-node resource name]
  (g/make-nodes (g/node-id->graph-id scene) [node [SpineSceneNode :name name :spine-scene resource]]
                (attach-spine-scene scene spine-scenes-node node)))

(defn- add-spine-scenes-handler [project {:keys [scene parent]} select-fn]
  (gui/query-and-add-resources!
   "Spine Scenes" [spineext/spine-scene-ext] (g/node-value parent :name-counts) project select-fn
   (partial add-spine-scene scene parent)))

(g/defnode SpineScenesNode
  (inherits outline/OutlineNode)
  (input names g/Str :array)
  (output name-counts gui/NameCounts :cached (g/fnk [names] (frequencies names)))
  (input build-errors g/Any :array)
  (output build-errors g/Any (gu/passthrough build-errors))
  (output node-outline outline/OutlineData :cached (gui/gen-outline-fnk "Spine Scenes" "Spine Scenes" 6 false []))
  (output add-handler-info g/Any
          (g/fnk [_node-id]
                 [_node-id "Spine Scenes..." spineext/spine-scene-icon add-spine-scenes-handler {}])))


;;//////////////////////////////////////////////////////////////////////////////////////////////


;; Loading a gui scene

(defn- load-gui-scene-spine [project self scene graph-id resource]
  (g/make-nodes graph-id [spine-scenes-node SpineScenesNode
                          no-spine-scene [SpineSceneNode
                                          :name ""]]
                (g/connect spine-scenes-node :_node-id self :nodes)
                (g/connect spine-scenes-node :build-errors self :build-errors)
                (g/connect spine-scenes-node :node-outline self :child-outlines)
                (g/connect spine-scenes-node :add-handler-info self :handler-infos)
                (attach-spine-scene self spine-scenes-node no-spine-scene true)
                (let [spine-scene-node-prop-keys (g/declared-property-labels SpineSceneNode)
                      resource-node-prop-keys (g/declared-property-labels gui/ResourceNode)
                      old-spine-scenes (for [spine-scene-desc (:spine-scenes scene)
                                             :let [spine-scene-desc (select-keys spine-scene-desc spine-scene-node-prop-keys)]]
                                         (g/make-nodes graph-id [spine-scene [SpineSceneNode
                                                                              :name (:name spine-scene-desc)
                                                                              :spine-scene (workspace/resolve-resource resource (:spine-scene spine-scene-desc))]]
                                                       (attach-spine-scene self spine-scenes-node spine-scene)))
                  ;; spine-scene-ext
                      new-spine-scenes (for [resources-desc (:resources scene)
                                             :let [resource-desc (select-keys resources-desc resource-node-prop-keys)]]
                                         (g/make-nodes graph-id [spine-scene [SpineSceneNode
                                                                              :name (:name resource-desc)
                                                                              :spine-scene (workspace/resolve-resource resource (:path resource-desc))]]
                                                       (when (str/ends-with? (:path resource-desc) spineext/spine-scene-ext)
                                                         (attach-spine-scene self spine-scenes-node spine-scene))))]
                  (concat old-spine-scenes new-spine-scenes))))

      
;;//////////////////////////////////////////////////////////////////////////////////////////////

(defn- fixup-spine-node [node-type-info node-desc]
  (let [node-type (:type node-desc)
        patch (if (= node-type :type-spine)
                {:type (:output-node-type node-type-info)
                 :custom-type (:output-custom-type node-type-info)}
                {})
        constants {:size [1.0 1.0 0.0 1.0]
                   :size-mode :size-mode-auto}
        out (merge node-desc patch constants)]
    out))

    
(defn- register-gui-resource-types! [workspace]
  (gui/register-gui-scene-loader! load-gui-scene-spine)
  (let [info {:node-type :type-custom
              :node-cls SpineNode
              :display-name "Spine"
              :custom-type (murmur/hash32 "Spine")
              :icon spineext/spine-scene-icon
              :convert-fn fixup-spine-node}
        info-depr (merge info {:node-type :type-spine
                               :custom-type 0
                               :output-node-type (:node-type info)
                               :output-custom-type (:custom-type info)
                               :deprecated true})]
    (gui/register-node-type-info! info)
    ; Register :type-spine with custom type 0 in order to be able to read old files
    (gui/register-node-type-info! info-depr)))

; The plugin
(defn load-plugin-spine-gui [workspace]
  (register-gui-resource-types! workspace))

(defn return-plugin []
  (fn [x] (load-plugin-spine-gui x)))
(return-plugin)
