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
            [editor.types :as types]
            [editor.util :as util]
            [util.coll :as coll]
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

(def ^:private validate-spine-scene (partial gui/validate-required-gui-resource "spine scene '%s' does not exist in the scene" :__spine_scene))

(defn- spine-scene-names [basic-gui-scene-info]
  (get-in basic-gui-scene-info [:gui-resource-kind-names :spine-scene]))

(defn- spine-scene-element-ids [basic-gui-scene-info]
  (get-in basic-gui-scene-info [:gui-resource-kind-basic-info :spine-scene]))

(defn- spine-scene-infos [costly-gui-scene-info]
  (get-in costly-gui-scene-info [:gui-resource-kind-costly-info :spine-scene]))

(defn- validate-spine-default-animation [node-id spine-scene-names spine-anim-ids spine-default-animation spine-scene]
  (when-not (g/error? (validate-spine-scene node-id spine-scene-names spine-scene))
    (gui/validate-optional-gui-resource "animation '%s' could not be found in the specified spine scene" :__spine_default_animation node-id spine-anim-ids spine-default-animation)))

(defn- validate-spine-skin [node-id spine-scene-names spine-skin-ids spine-skin spine-scene]
  (when-not (g/error? (validate-spine-scene node-id spine-scene-names spine-scene))
    (spineext/validate-skin node-id :__spine_skin spine-skin-ids spine-skin)))

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

(g/defnk produce-spine-updatable [_node-id spine-data-handle spine-default-animation spine-skin spine-vertex-buffer]
  (when (and (some? spine-data-handle)
             spine-default-animation
             spine-skin)
    {:node-id _node-id
     :name "Spine GUI Updater"
     :update-fn (fn [state {:keys [dt]}]
                  (let [vb (produce-local-vertices spine-data-handle spine-skin spine-default-animation dt)]
                    (assoc state :spine-vertex-buffer vb)))
     :initial-state {:spine-vertex-buffer spine-vertex-buffer}}))

(defn- renderable->vertices [renderable]
  (let [handle (spineext/renderable->handle renderable)
        world-transform (:world-transform renderable)
        per-node-user-data (:user-data renderable)
        vertex-buffer (or (get-in renderable [:updatable :state :spine-vertex-buffer])
                          (:spine-vertex-buffer per-node-user-data))
        color (:color per-node-user-data)
        vb-data-transformed (map (fn [vtx] (transform-vtx world-transform color vtx)) vertex-buffer)]
    vb-data-transformed))

(defn- gen-vb [user-data renderables]
  (let [vertices (mapcat (fn [renderable] (renderable->vertices renderable)) renderables)
        vb-out (spineext/generate-vertex-buffer vertices)]
    vb-out))

(defn- aabb->rect-lines [aabb]
  (let [minp (types/min-p aabb)
        maxp (types/max-p aabb)
        [x0 y0 _] (types/Point3d->Vec3 minp)
        [x1 y1 _] (types/Point3d->Vec3 maxp)]
    [[x0 y0 0] [x1 y0 0]
     [x1 y0 0] [x1 y1 0]
     [x1 y1 0] [x0 y1 0]
     [x0 y1 0] [x0 y0 0]]))

(def ^:private spine-custom-property-infos
  [{:legacy-field :spine-scene
    :protobuf-type :type-string
    :value-field :string-value
    :registration {:id "spine_scene"
                   :type g/Str
                   :default ""
                   :edit-type-fnk (g/fnk [basic-gui-scene-info]
                                    (gui/required-gui-resource-choicebox (spine-scene-names basic-gui-scene-info)))
                   :error-fnk (g/fnk [_node-id basic-gui-scene-info spine-scene]
                                (validate-spine-scene _node-id (spine-scene-names basic-gui-scene-info) spine-scene))
                   :resource-kind :spine-scene
                   :label "Spine Scene"}}
   {:legacy-field :spine-default-animation
    :protobuf-type :type-string
    :value-field :string-value
    :registration {:id "spine_default_animation"
                   :type g/Str
                   :default ""
                   :edit-type-fnk (g/fnk [spine-anim-ids]
                                    (gui/optional-gui-resource-choicebox spine-anim-ids))
                   :error-fnk (g/fnk [_node-id basic-gui-scene-info spine-anim-ids spine-default-animation spine-scene]
                                (validate-spine-default-animation _node-id (spine-scene-names basic-gui-scene-info) spine-anim-ids spine-default-animation spine-scene))
                   :label "Default Animation"}}
   {:legacy-field :spine-skin
    :protobuf-type :type-string
    :value-field :string-value
    :registration {:id "spine_skin"
                   :type g/Str
                   :default ""
                   :edit-type-fnk (g/fnk [spine-skin-ids]
                                    (spineext/->skin-choicebox spine-skin-ids))
                   :error-fnk (g/fnk [_node-id basic-gui-scene-info spine-scene spine-skin spine-skin-ids]
                                (validate-spine-skin _node-id (spine-scene-names basic-gui-scene-info) spine-skin-ids spine-skin spine-scene))
                   :label "Skin"}}
   {:legacy-field :spine-create-bones
    :protobuf-type :type-boolean
    :value-field :boolean
    :registration {:id "spine_create_bones"
                   :type g/Bool
                   :default false
                   :label "Create Bones"}}])

(def ^:private spine-custom-property-id->info
  (coll/pair-map-by (comp :id :registration) spine-custom-property-infos))

(defn- spine-custom-property [custom-property-id value]
  (let [{:keys [protobuf-type value-field]} (spine-custom-property-id->info custom-property-id)]
    {:id custom-property-id
     :type protobuf-type
     value-field value}))

(defn- custom-property-id [custom-property]
  (:id custom-property))

(def ^:private legacy-spine-node-desc-field->custom-property-id
  (into {}
        (map (fn [{:keys [legacy-field registration]}]
               [legacy-field (:id registration)]))
        spine-custom-property-infos))

(def ^:private legacy-spine-node-desc-field->default
  (into {}
        (map (fn [{:keys [legacy-field registration]}]
               [legacy-field (:default registration)]))
        spine-custom-property-infos))

(def ^:private node-desc-pb-field->index
  (reduce-kv (fn [pb-field->index index pb-field]
               (assoc pb-field->index pb-field index))
             {}
             (protobuf/fields-by-indices Gui$NodeDesc)))

(def ^:private custom-properties-pb-field-index
  (node-desc-pb-field->index :custom-properties))

(def ^:private legacy-spine-pb-field-indices
  (coll/pair-map-by node-desc-pb-field->index (keys legacy-spine-node-desc-field->custom-property-id)))

(defn- migrate-legacy-spine-overridden-fields [overridden-fields]
  (vec
    (sort
      (into #{}
            (map (fn [overridden-field]
                   (if (contains? legacy-spine-pb-field-indices overridden-field)
                     custom-properties-pb-field-index
                     overridden-field)))
            overridden-fields))))

(g/defnk produce-spine-node-msg [visual-base-node-msg ^:raw clipping-mode ^:raw clipping-visible ^:raw clipping-inverted]
  (merge visual-base-node-msg
         (protobuf/make-map-without-defaults Gui$NodeDesc
           :size-mode :size-mode-auto
           :clipping-mode clipping-mode
           :clipping-visible clipping-visible
           :clipping-inverted clipping-inverted)))

(g/defnode SpineNode
  (inherits gui/VisualNode)

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
                       [:__spine_scene :__spine_default_animation :__spine_skin :__spine_create_bones :color :alpha :inherit-alpha :layer :blend-mode :pivot :x-anchor :y-anchor
                        :adjust-mode :clipping :visible-clipper :inverted-clipper]))

  (output node-msg g/Any :cached produce-spine-node-msg)
  (output spine-scene g/Str
          (g/fnk [prop->value]
            (get prop->value :__spine_scene "")))
  (output spine-default-animation g/Str
          (g/fnk [prop->value]
            (get prop->value :__spine_default_animation "")))
  (output spine-skin g/Str
          (g/fnk [prop->value]
            (get prop->value :__spine_skin "")))
  (output spine-create-bones g/Bool
          (g/fnk [prop->value]
            (get prop->value :__spine_create_bones false)))
  (output spine-anim-ids gui/GuiResourceNames
          (g/fnk [basic-gui-scene-info spine-scene]
            (let [element-ids (spine-scene-element-ids basic-gui-scene-info)]
              (:spine-anim-ids (or (get element-ids spine-scene)
                                   (get element-ids ""))))))
  (output spine-skin-ids gui/GuiResourceNames
          (g/fnk [basic-gui-scene-info spine-scene]
            (let [element-ids (spine-scene-element-ids basic-gui-scene-info)]
              (:spine-skin-ids (or (get element-ids spine-scene)
                                   (get element-ids ""))))))
  (output spine-scene-scene g/Any
          (g/fnk [costly-gui-scene-info spine-scene]
            (let [infos (spine-scene-infos costly-gui-scene-info)]
              (:spine-scene-scene (or (get infos spine-scene)
                                      (get infos ""))))))
  (output spine-scene-bones g/Any
          (g/fnk [costly-gui-scene-info spine-scene]
            (let [infos (spine-scene-infos costly-gui-scene-info)]
              (:spine-bones (or (get infos spine-scene)
                                (get infos ""))))))
  (output spine-scene-pb g/Any
          (g/fnk [costly-gui-scene-info spine-scene]
            (let [infos (spine-scene-infos costly-gui-scene-info)]
              (:spine-scene-pb (or (get infos spine-scene)
                                   (get infos ""))))))

  ;; The handle to the C++ resource
  (output spine-data-handle g/Any
          (g/fnk [costly-gui-scene-info spine-scene]
            (let [infos (spine-scene-infos costly-gui-scene-info)]
              (:spine-data-handle (or (get infos spine-scene)
                                      (get infos ""))))))

  (output spine-vertex-buffer g/Any :cached (g/fnk [spine-scene spine-data-handle spine-skin spine-default-animation]
                                                   (produce-local-vertices spine-data-handle spine-skin spine-default-animation 0.0)))

  (output aabb g/Any
          (g/fnk [spine-data-handle]
            (if spine-data-handle
              (spineext/handle->aabb spine-data-handle)
              geom/empty-bounding-box)))

  ; Overloaded outputs from VisualNode
  (output scene-updatable g/Any :cached produce-spine-updatable)
  (output gpu-texture TextureLifecycle (g/constantly nil))
  (output scene-renderable-user-data g/Any :cached (g/fnk [aabb spine-scene-scene spine-vertex-buffer color+alpha clipping-mode clipping-inverted clipping-visible]
                                                          (let [lines (aabb->rect-lines aabb)
                                                                user-data (assoc (get-in spine-scene-scene [:renderable :user-data])
                                                                                 :color color+alpha
                                                                                 :renderable-tags #{:gui-spine}
                                                                                 :gen-vb gen-vb
                                                                                 :spine-vertex-buffer spine-vertex-buffer
                                                                                 :line-data lines)]
                                                            (cond-> user-data
                                                              (not= :clipping-mode-none clipping-mode)
                                                              (assoc :clipping {:mode clipping-mode :inverted clipping-inverted :visible clipping-visible})))))

  (output own-build-errors g/Any
          (g/fnk [_node-id basic-gui-scene-info build-errors-visual-node spine-anim-ids spine-default-animation spine-skin-ids spine-skin spine-scene]
            (let [spine-scene-names (spine-scene-names basic-gui-scene-info)]
              (g/package-errors
                _node-id
                build-errors-visual-node
                (validate-spine-scene _node-id spine-scene-names spine-scene)
                (validate-spine-default-animation _node-id spine-scene-names spine-anim-ids spine-default-animation spine-scene)
                (validate-spine-skin _node-id spine-scene-names spine-skin-ids spine-skin spine-scene))))))

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
  (output node-outline outline/OutlineData :cached (g/fnk [_node-id name spine-scene-resource build-errors]
                                                     (cond-> {:node-id _node-id
                                                              :node-outline-key name
                                                              :label name
                                                              :icon spineext/spine-scene-icon
                                                              :outline-error? (g/error-fatal? build-errors)}

                                                             (resource/resource? spine-scene-resource)
                                                             (assoc :link spine-scene-resource :outline-show-link? true))))
  (output pb-msg g/Any (g/fnk [name spine-scene]
                              {:name name
                               :path (resource/resource->proj-path spine-scene)}))
  (output resource-kind-basic-info g/Any :cached produce-spine-scene-element-ids)
  (output resource-kind-costly-info g/Any :cached produce-spine-scene-infos)
  (output build-errors g/Any (g/fnk [_node-id name name-counts spine-scene]
                                    (g/package-errors _node-id
                                                      (gui/prop-unique-id-error _node-id :name name name-counts "Name")
                                                      (gui/prop-resource-error _node-id :spine-scene spine-scene "Spine Scene")))))

;;//////////////////////////////////////////////////////////////////////////////////////////////

(defn- gui-scene-node->spine-resource-kind-node [basis gui-scene-node]
  (gui/gui-resource-kind-node basis gui-scene-node :spine-scene))

(defmethod ext-graph/init-attachment ::SpineSceneNode [evaluation-context rt project parent-node-id _ child-node-id attachment]
  (-> attachment
      (util/provide-defaults
        "name" (ext-graph/gen-gui-component-name attachment "spine_scene" gui-scene-node->spine-resource-kind-node rt parent-node-id evaluation-context))
      (ext-graph/attachment->set-tx-steps child-node-id rt project evaluation-context)))

(defn- fixup-spine-node [node-type-info node-desc]
  (let [node-type (:type node-desc)
        legacy-spine-node (identical? :type-spine node-type)
        overridden-fields (set (:overridden-fields node-desc))
        custom-property-ids (into #{} (keep custom-property-id) (:custom-properties node-desc))
        custom-properties
        (reduce
          (fn [custom-properties [node-desc-field custom-property-id]]
            (let [pb-field-index (node-desc-pb-field->index node-desc-field)
                  overridden-field? (contains? overridden-fields pb-field-index)
                  default-value (legacy-spine-node-desc-field->default node-desc-field)
                  value (get node-desc node-desc-field default-value)]
              (if (or (contains? custom-property-ids custom-property-id)
                      (not (or overridden-field?
                               (contains? node-desc node-desc-field)
                               (and legacy-spine-node (= :spine-scene node-desc-field))
                               (not= default-value value))))
                custom-properties
                (conj custom-properties (spine-custom-property custom-property-id value)))))
          (vec (:custom-properties node-desc))
          legacy-spine-node-desc-field->custom-property-id)
        migrate-overridden-fields? (some #(contains? legacy-spine-pb-field-indices %) overridden-fields)]
    (cond-> (cond-> (-> node-desc
                        (assoc :size-mode :size-mode-auto)
                        (dissoc
                          :spine-scene
                          :spine-default-animation
                          :spine-skin
                          :spine-create-bones
                          :spine-node-child))
                    (seq custom-properties)
                    (assoc :custom-properties custom-properties)
                    migrate-overridden-fields?
                    (update :overridden-fields migrate-legacy-spine-overridden-fields))

            (= :type-spine node-type)
            (assoc
              :type (:output-node-type node-type-info)
              :custom-type (:output-custom-type node-type-info)))))

(node-types/register-node-type-name! SpineNode "gui-node-type-spine")

(defn- register-gui-resource-types! [workspace]
  (let [info {:node-type :type-custom
              :node-cls SpineNode
              :display-name "Spine"
              :custom-type-name "Spine"
              :icon spineext/spine-scene-icon
              :convert-fn fixup-spine-node
              :defaults gui/visual-base-node-defaults
              :custom-properties (mapv :registration spine-custom-property-infos)}
        info-depr (merge info {:node-type :type-spine
                               :custom-type 0
                               :output-node-type (:node-type info)
                               :output-custom-type (murmur/hash32 "Spine")
                               :deprecated true})]
    (g/transact
      (concat
        (gui/register-node-type-info workspace info)
        ;; Register :type-spine with custom type 0 in order to be able to read old files.
        (gui/register-node-type-info workspace info-depr)
        (gui/register-node-tree-attachment-node-type workspace SpineNode)
        (gui/register-gui-resource-kind
          workspace :spine-scene
          {:label "Spine Scenes"
           :icon spineext/spine-scene-icon
           :exts [spineext/spine-scene-ext]
           :node-type SpineSceneNode
           :resource-property :spine-scene
           :attachment-property :spine-scenes
           :attach-fn gui/connect-gui-resource-kind-entry})))))

; The plugin
(defn load-plugin-spine-gui [workspace]
  (register-gui-resource-types! workspace))

(defn return-plugin []
  (fn [x] (load-plugin-spine-gui x)))
(return-plugin)
