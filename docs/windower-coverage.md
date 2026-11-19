# Windower API coverage

Made by `python tools/windower_coverage.py` on 19 November 2026 (VanaCore 0.8.275). Regenerate it after changing compat/windower.lua.

153 addons from the Windower addons in a VanaCore install (addons\windower, TrustSupport among them) and github.com/Windower/Lua (live branch, 5a35aef) addons; libraries from addons\windower\libs. Windower's API definitions from the same repository (definitions/windower.lua).

**123 of 153 use nothing VanaCore lacks** (by name: what a call returns is not checked).


## Missing (the call raises "not supported by VanaCore yet")

| API | addons | which (* only through a library) |
|---|---|---|
| `windower.ffxi.pass_item` | 2 | lottery, Treasury |
| `windower.ffxi.put_item` | 2 | craft, latentchecker |
| `windower.ffxi.remove_blue_magic_spell` | 2 | azureSets, bluguide |
| `windower.ffxi.set_blue_magic_spell` | 2 | azureSets, bluguide |
| `windower.ffxi.get_camera` | 1 | InfoReplacer |
| `windower.ffxi.get_mob_list` | 1 | organizer |
| `windower.ffxi.lot_item` | 1 | Treasury |
| `windower.ffxi.move_item` | 1 | itemizer |
| `windower.ffxi.reset_attachments` | 1 | autocontrol |
| `windower.ffxi.reset_blue_magic_spells` | 1 | azureSets |
| `windower.ffxi.set_attachment` | 1 | autocontrol |
| `windower.ffxi.stack_items` | 1 | itemizer |
| `windower.net` | 1 | linker |

## Stubs (there, but empty or returning a constant)

| API | addons | which (* only through a library) |
|---|---|---|
| `windower.debug` | 3 | battlemod, GearSwap, organizer |
| `windower.console.visible` | 1 | ConsoleBG |
| `windower.take_focus` | 1 | switch_focus |

## Events VanaCore never raises

| event | addons | which (* only through a library) |
|---|---|---|
| `chat message` | 5 | answeringmachine, autoinvite, chatmon, respond, whereisdi |
| `add item` | 4 | EmpyPopTracker, giltracker, invtracker, Lookup |
| `remove item` | 3 | EmpyPopTracker, giltracker, invtracker |
| `linkshell change` | 2 | chatPorter, invtracker |
| `party invite` | 2 | autojoin, chatmon |
| `emote` | 1 | chatmon |
| `examined` | 1 | chatmon |
| `level down` | 1 | barfiller |
| `level up` | 1 | barfiller |

## Requires that find nothing (not the addon's own file, a library in the libs folder, or built in)

A name starting with _ is usually one of Windower's native modules (a DLL beside Windower.exe).

| module | addons |
|---|---|
| `_XICamera` | 1: XICamera |
| `_XIPivot` | 1: XIPivot |
| `sqlite3` | 1: InfoBar |
| `ssl.config` | 1: whereisdi |
| `ssl.context` | 1: whereisdi |
| `ssl.core` | 1: whereisdi |
| `ssl.x509` | 1: whereisdi |

## In Windower's definitions, lacking here, no addon above uses

`windower.chat.add_to_input` (stub), `windower.chat.get_input` (stub), `windower.chat.paste` (stub), `windower.chat.set_input` (stub), `windower.console.clear` (stub), `windower.console.close` (stub), `windower.console.open` (stub), `windower.console.set_position` (stub), `windower.get_camera` (missing), `windower.get_chat_filters` (stub), `windower.get_item_display` (missing)

## Result fields windower.lua never sets (from the definitions)

Addons that read a field of that name anywhere (a rough guide: the name may be another table's).

| result | field | addons |
|---|---|---|
| `ffxi.get_player().merits` | `string` | 28: autoex, autoinvite, barfiller, Bars, battlemod, blist, bluguide, Bonanza, ... (+20) |
| `ffxi.get_player().merits` | `blue` | 25: autocontrol, Balloon, Bars, bluguide, chatPorter, ConsoleBG, Debuffed, EmpyPopTracker, ... (+17) |
| `get_item_display` | `bag` | 9: Dimmer, findAll, GearSwap, itemizer, MyCap, MyExp, MyHome, organizer, ... (+1) |
| `ffxi.get_player().merits` | `str` | 8: GearSwap, hasteinfo, InfoBar, ohShi, parse, shortcuts, timestamp, translate |
| `ffxi.get_player` | `main_job_full` | 4: DressUp, GearSwap, Lookup, pointwatch |
| `ffxi.get_player().merits` | `wind` | 3: autocontrol, Bars, InfoReplacer |
| `ffxi.get_player` | `sub_job_full` | 2: GearSwap, Lookup |
| `ffxi.get_player().merits` | `dark` | 2: autocontrol, Bars |
| `ffxi.get_player().skills` | `alchemy` | 2: InfoBar, InfoReplacer |
| `ffxi.get_player().skills` | `bonecraft` | 2: InfoBar, InfoReplacer |
| `ffxi.get_player().skills` | `clothcraft` | 2: InfoBar, InfoReplacer |
| `ffxi.get_player().skills` | `cooking` | 2: InfoBar, InfoReplacer |
| `ffxi.get_player().skills` | `fishing` | 2: InfoBar, InfoReplacer |
| `ffxi.get_player().skills` | `goldsmithing` | 2: InfoBar, InfoReplacer |
| `ffxi.get_player().skills` | `leathercraft` | 2: InfoBar, InfoReplacer |
| `ffxi.get_player().skills` | `smithing` | 2: InfoBar, InfoReplacer |
| `ffxi.get_player().skills` | `woodworking` | 2: InfoBar, InfoReplacer |
| `get_windower_settings` | `ffxi_version` | 2: boxdestroyer, diagnostics |
| `ffxi.get_info` | `target_arrow` | 1: GearSwap |
| `ffxi.get_player().merits` | `accuracy` | 1: digger |
| `ffxi.get_player().merits` | `agi` | 1: GearSwap |
| `ffxi.get_player().merits` | `archery` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `assimilation` | 1: bluguide |
| `ffxi.get_player().merits` | `axe` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `chr` | 1: GearSwap |
| `ffxi.get_player().merits` | `club` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `dagger` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `dex` | 1: GearSwap |
| `ffxi.get_player().merits` | `evasion` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `fold` | 1: rolltracker |
| `ffxi.get_player().merits` | `great_axe` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `great_katana` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `great_sword` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `guarding` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `hand_to_hand` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `healing` | 1: GearSwap |
| `ffxi.get_player().merits` | `int` | 1: GearSwap |
| `ffxi.get_player().merits` | `katana` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `marksmanship` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `mnd` | 1: GearSwap |
| `ffxi.get_player().merits` | `parrying` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `polearm` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `scythe` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `shield` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `singing` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `snapshot` | 1: Overlay |
| `ffxi.get_player().merits` | `staff` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `sword` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `throwing` | 1: InfoReplacer |
| `ffxi.get_player().merits` | `vit` | 1: GearSwap |
| `ffxi.get_player().skills` | `archery` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `axe` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `blue_magic` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `club` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `dagger` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `dark_magic` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `divine_magic` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `elemental_magic` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `enfeebling_magic` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `enhancing_magic` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `evasion` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `geomancy` | 1: hasteinfo |
| `ffxi.get_player().skills` | `great_axe` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `great_katana` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `great_sword` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `hand_to_hand` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `healing_magic` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `katana` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `marksmanship` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `parrying` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `polearm` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `scythe` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `shield` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `singing` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `staff` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `summoning_magic` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `sword` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `synergy` | 1: InfoReplacer |
| `ffxi.get_player().skills` | `throwing` | 1: InfoReplacer |
| `get_item_display` | `augment` | 1: GearSwap |
| `get_item_display` | `signature` | 1: TrustSupport |
| `get_windower_settings` | `branch` | 1: diagnostics |
| `get_windower_settings` | `hook_version` | 1: diagnostics |
| `ffxi.get_player().merits` | `activate_recast` | 0:  |
| `ffxi.get_player().merits` | `aggressive_aim` | 0:  |
| `ffxi.get_player().merits` | `aggressor_recast` | 0:  |
| `ffxi.get_player().merits` | `altruism` | 0:  |
| `ffxi.get_player().merits` | `ambush` | 0:  |
| `ffxi.get_player().merits` | `ancient_circle_recast` | 0:  |
| `ffxi.get_player().merits` | `ancient_magic_magic_attack_bonus` | 0:  |
| `ffxi.get_player().merits` | `ancient_magic_magic_burst_damage` | 0:  |
| `ffxi.get_player().merits` | `angon` | 0:  |
| `ffxi.get_player().merits` | `animus_misery_effect` | 0:  |
| `ffxi.get_player().merits` | `animus_solace_effect` | 0:  |
| `ffxi.get_player().merits` | `apex_arrow` | 0:  |
| `ffxi.get_player().merits` | `arcane_circle_recast` | 0:  |
| `ffxi.get_player().merits` | `aspir_absorption_amount` | 0:  |
| `ffxi.get_player().merits` | `assassins_charge` | 0:  |
| `ffxi.get_player().merits` | `aura_steal` | 0:  |
| `ffxi.get_player().merits` | `automaton_magic_skill` | 0:  |
| `ffxi.get_player().merits` | `automaton_melee_skill` | 0:  |
| `ffxi.get_player().merits` | `automaton_ranged_skill` | 0:  |
| `ffxi.get_player().merits` | `avatar_magical_accuracy` | 0:  |
| `ffxi.get_player().merits` | `avatar_magical_attack` | 0:  |
| `ffxi.get_player().merits` | `avatar_physical_accuracy` | 0:  |
| `ffxi.get_player().merits` | `avatar_physical_attack` | 0:  |
| `ffxi.get_player().merits` | `banish_effect` | 0:  |
| `ffxi.get_player().merits` | `bar_spell_effect` | 0:  |
| `ffxi.get_player().merits` | `battuta` | 0:  |
| `ffxi.get_player().merits` | `beast_affinity` | 0:  |
| `ffxi.get_player().merits` | `beast_healer` | 0:  |
| `ffxi.get_player().merits` | `berserk_recast` | 0:  |
| `ffxi.get_player().merits` | `blade_bash` | 0:  |
| `ffxi.get_player().merits` | `blade_shun` | 0:  |
| `ffxi.get_player().merits` | `blaze_of_glory_recast` | 0:  |
| `ffxi.get_player().merits` | `building_flourish_effect` | 0:  |
| `ffxi.get_player().merits` | `burst_affinity_recast` | 0:  |
| `ffxi.get_player().merits` | `bust_duration` | 0:  |
| `ffxi.get_player().merits` | `call_beast_recast` | 0:  |
| `ffxi.get_player().merits` | `camouflage_recast` | 0:  |
| `ffxi.get_player().merits` | `chain_affinity_recast` | 0:  |
| `ffxi.get_player().merits` | `chakra_recast` | 0:  |
| `ffxi.get_player().merits` | `chivalry` | 0:  |
| `ffxi.get_player().merits` | `closed_position` | 0:  |
| `ffxi.get_player().merits` | `con_anima` | 0:  |
| `ffxi.get_player().merits` | `cont_brio` | 0:  |
| `ffxi.get_player().merits` | `convergence` | 0:  |
| `ffxi.get_player().merits` | `convert_recast` | 0:  |
| `ffxi.get_player().merits` | `counter_rate` | 0:  |
| `ffxi.get_player().merits` | `cover_effect_length` | 0:  |
| `ffxi.get_player().merits` | `critical_hit_rate` | 0:  |
| `ffxi.get_player().merits` | `curative_recantation` | 0:  |
| `ffxi.get_player().merits` | `cure_cast_time` | 0:  |
| `ffxi.get_player().merits` | `dark_seal` | 0:  |
| `ffxi.get_player().merits` | `deep_breathing` | 0:  |
| `ffxi.get_player().merits` | `defender_recast` | 0:  |
| `ffxi.get_player().merits` | `dematerialize_recast` | 0:  |
| `ffxi.get_player().merits` | `desperate_blows` | 0:  |
| `ffxi.get_player().merits` | `devotion` | 0:  |
| `ffxi.get_player().merits` | `diabolic_eye` | 0:  |
| `ffxi.get_player().merits` | `diffusion` | 0:  |
| `ffxi.get_player().merits` | `divine` | 0:  |
| `ffxi.get_player().merits` | `divine_seal_recast` | 0:  |
| `ffxi.get_player().merits` | `dodge_recast` | 0:  |
| `ffxi.get_player().merits` | `doton_effect` | 0:  |
| `ffxi.get_player().merits` | `double_attack_rate` | 0:  |
| `ffxi.get_player().merits` | `earth_magic_accuracy` | 0:  |
| `ffxi.get_player().merits` | `earth_magic_potency` | 0:  |
| `ffxi.get_player().merits` | `ecliptic_attrition_effect` | 0:  |
| `ffxi.get_player().merits` | `elemental` | 0:  |
| `ffxi.get_player().merits` | `elemental_magic_debuff_duration` | 0:  |
| `ffxi.get_player().merits` | `elemental_magic_debuff_effect` | 0:  |
| `ffxi.get_player().merits` | `elemental_magic_magic_accuracy` | 0:  |
| `ffxi.get_player().merits` | `elemental_seal_recast` | 0:  |
| `ffxi.get_player().merits` | `empathy` | 0:  |
| `ffxi.get_player().merits` | `enchainment` | 0:  |
| `ffxi.get_player().merits` | `enemy_critical_hit_rate` | 0:  |
| `ffxi.get_player().merits` | `enfeebling` | 0:  |
| `ffxi.get_player().merits` | `enfeebling_magic_duration` | 0:  |
| `ffxi.get_player().merits` | `enhancing` | 0:  |
| `ffxi.get_player().merits` | `enhancing_magic_duration` | 0:  |
| `ffxi.get_player().merits` | `enlightenment` | 0:  |
| `ffxi.get_player().merits` | `enmity_decrease` | 0:  |
| `ffxi.get_player().merits` | `enmity_increase` | 0:  |
| `ffxi.get_player().merits` | `enspell_damage` | 0:  |
| `ffxi.get_player().merits` | `entropy` | 0:  |
| `ffxi.get_player().merits` | `equanimity` | 0:  |
| `ffxi.get_player().merits` | `exenterator` | 0:  |
| `ffxi.get_player().merits` | `fan_dance` | 0:  |
| `ffxi.get_player().merits` | `fealty` | 0:  |
| `ffxi.get_player().merits` | `feint` | 0:  |
| `ffxi.get_player().merits` | `feral_howl` | 0:  |
| `ffxi.get_player().merits` | `finale_recast` | 0:  |
| `ffxi.get_player().merits` | `fine_tuning` | 0:  |
| `ffxi.get_player().merits` | `fire_magic_accuracy` | 0:  |
| `ffxi.get_player().merits` | `fire_magic_potency` | 0:  |
| `ffxi.get_player().merits` | `flashy_shot` | 0:  |
| `ffxi.get_player().merits` | `flee_recast` | 0:  |
| `ffxi.get_player().merits` | `focalization` | 0:  |
| `ffxi.get_player().merits` | `focus_recast` | 0:  |
| `ffxi.get_player().merits` | `formless_strikes` | 0:  |
| `ffxi.get_player().merits` | `full_circle_effect` | 0:  |
| `ffxi.get_player().merits` | `gambit_recast` | 0:  |
| `ffxi.get_player().merits` | `geocrush` | 0:  |
| `ffxi.get_player().merits` | `grand_fall` | 0:  |
| `ffxi.get_player().merits` | `grimoire_recast` | 0:  |
| `ffxi.get_player().merits` | `guardian` | 0:  |
| `ffxi.get_player().merits` | `haste_samba_effect` | 0:  |
| `ffxi.get_player().merits` | `heavenly_strike` | 0:  |
| `ffxi.get_player().merits` | `helix_magic_attack` | 0:  |
| `ffxi.get_player().merits` | `hide_recast` | 0:  |
| `ffxi.get_player().merits` | `high_jump_recast` | 0:  |
| `ffxi.get_player().merits` | `holy_circle_recast` | 0:  |
| `ffxi.get_player().merits` | `huton_effect` | 0:  |
| `ffxi.get_player().merits` | `hyoton_effect` | 0:  |
| `ffxi.get_player().merits` | `ice_magic_accuracy` | 0:  |
| `ffxi.get_player().merits` | `ice_magic_potency` | 0:  |
| `ffxi.get_player().merits` | `ikishoten` | 0:  |
| `ffxi.get_player().merits` | `immunobreak_chance` | 0:  |
| `ffxi.get_player().merits` | `innin_effect` | 0:  |
| `ffxi.get_player().merits` | `inspiration` | 0:  |
| `ffxi.get_player().merits` | `invigorate` | 0:  |
| `ffxi.get_player().merits` | `iron_will` | 0:  |
| `ffxi.get_player().merits` | `jump_recast` | 0:  |
| `ffxi.get_player().merits` | `katon_effect` | 0:  |
| `ffxi.get_player().merits` | `kick_attack_rate` | 0:  |
| `ffxi.get_player().merits` | `killer_effects` | 0:  |
| `ffxi.get_player().merits` | `killer_instinct` | 0:  |
| `ffxi.get_player().merits` | `last_resort_effect` | 0:  |
| `ffxi.get_player().merits` | `last_resort_recast` | 0:  |
| `ffxi.get_player().merits` | `last_stand` | 0:  |
| `ffxi.get_player().merits` | `lightning_magic_accuracy` | 0:  |
| `ffxi.get_player().merits` | `lightning_magic_potency` | 0:  |
| `ffxi.get_player().merits` | `loaded_deck` | 0:  |
| `ffxi.get_player().merits` | `lullaby_recast` | 0:  |
| `ffxi.get_player().merits` | `lunge_effect` | 0:  |
| `ffxi.get_player().merits` | `madrigal_effect` | 0:  |
| `ffxi.get_player().merits` | `magic_accuracy` | 0:  |
| `ffxi.get_player().merits` | `magical_accuracy` | 0:  |
| `ffxi.get_player().merits` | `mantra` | 0:  |
| `ffxi.get_player().merits` | `martyr` | 0:  |
| `ffxi.get_player().merits` | `max_sublimation` | 0:  |
| `ffxi.get_player().merits` | `maximum_merit_points` | 0:  |
| `ffxi.get_player().merits` | `meditate_recast` | 0:  |
| `ffxi.get_player().merits` | `mending_hallation` | 0:  |
| `ffxi.get_player().merits` | `meteor_strike` | 0:  |
| `ffxi.get_player().merits` | `minne_effect` | 0:  |
| `ffxi.get_player().merits` | `minuet_effect` | 0:  |
| `ffxi.get_player().merits` | `modus_veritas_duration` | 0:  |
| `ffxi.get_player().merits` | `monster_correlation` | 0:  |
| `ffxi.get_player().merits` | `muted_soul` | 0:  |
| `ffxi.get_player().merits` | `nightingale` | 0:  |
| `ffxi.get_player().merits` | `ninja_tool_expertise` | 0:  |
| `ffxi.get_player().merits` | `ninjutsu` | 0:  |
| `ffxi.get_player().merits` | `ninjutsu_magic_accuracy` | 0:  |
| `ffxi.get_player().merits` | `ninjutsu_magic_attack_bonus` | 0:  |
| `ffxi.get_player().merits` | `no_foot_rise` | 0:  |
| `ffxi.get_player().merits` | `optimization` | 0:  |
| `ffxi.get_player().merits` | `overwhelm` | 0:  |
| `ffxi.get_player().merits` | `penance` | 0:  |
| `ffxi.get_player().merits` | `pflug_effect` | 0:  |
| `ffxi.get_player().merits` | `phantom_roll_recast` | 0:  |
| `ffxi.get_player().merits` | `physical_accuracy` | 0:  |
| `ffxi.get_player().merits` | `primeval_zeal` | 0:  |
| `ffxi.get_player().merits` | `quick_draw_accuracy` | 0:  |
| `ffxi.get_player().merits` | `quick_draw_recast` | 0:  |
| `ffxi.get_player().merits` | `radial_arcana` | 0:  |
| `ffxi.get_player().merits` | `raiton_effect` | 0:  |
| `ffxi.get_player().merits` | `rampart_recast` | 0:  |
| `ffxi.get_player().merits` | `random_deal_recast` | 0:  |
| `ffxi.get_player().merits` | `rapid_shot_rate` | 0:  |
| `ffxi.get_player().merits` | `rayke` | 0:  |
| `ffxi.get_player().merits` | `realmrazer` | 0:  |
| `ffxi.get_player().merits` | `recycle` | 0:  |
| `ffxi.get_player().merits` | `regen_effect` | 0:  |
| `ffxi.get_player().merits` | `repair_recast` | 0:  |
| `ffxi.get_player().merits` | `requiescat` | 0:  |
| `ffxi.get_player().merits` | `resolution` | 0:  |
| `ffxi.get_player().merits` | `reverse_flourish_effect` | 0:  |
| `ffxi.get_player().merits` | `reward_recast` | 0:  |
| `ffxi.get_player().merits` | `role_reversal` | 0:  |
| `ffxi.get_player().merits` | `ruinator` | 0:  |
| `ffxi.get_player().merits` | `rune_enchantment_effect` | 0:  |
| `ffxi.get_player().merits` | `saber_dance` | 0:  |
| `ffxi.get_player().merits` | `sange` | 0:  |
| `ffxi.get_player().merits` | `savagery` | 0:  |
| `ffxi.get_player().merits` | `scavenge_effect` | 0:  |
| `ffxi.get_player().merits` | `sentinel_recast` | 0:  |
| `ffxi.get_player().merits` | `sharpshot_recast` | 0:  |
| `ffxi.get_player().merits` | `shattersoul` | 0:  |
| `ffxi.get_player().merits` | `shield_bash_recast` | 0:  |
| `ffxi.get_player().merits` | `shijin_spiral` | 0:  |
| `ffxi.get_player().merits` | `shikikoyo` | 0:  |
| `ffxi.get_player().merits` | `sic_recast` | 0:  |
| `ffxi.get_player().merits` | `sleight_of_sword` | 0:  |
| `ffxi.get_player().merits` | `snake_eye` | 0:  |
| `ffxi.get_player().merits` | `sneak_attack_recast` | 0:  |
| `ffxi.get_player().merits` | `souleater_recast` | 0:  |
| `ffxi.get_player().merits` | `spell_interruption_rate` | 0:  |
| `ffxi.get_player().merits` | `spirit_link_recast` | 0:  |
| `ffxi.get_player().merits` | `stardiver` | 0:  |
| `ffxi.get_player().merits` | `stealth_shot` | 0:  |
| `ffxi.get_player().merits` | `step_accuracy` | 0:  |
| `ffxi.get_player().merits` | `store_tp_effect` | 0:  |
| `ffxi.get_player().merits` | `stormsurge` | 0:  |
| `ffxi.get_player().merits` | `strafe` | 0:  |
| `ffxi.get_player().merits` | `subtle_blow_effect` | 0:  |
| `ffxi.get_player().merits` | `suiton_effect` | 0:  |
| `ffxi.get_player().merits` | `summoning` | 0:  |
| `ffxi.get_player().merits` | `summoning_magic_cast_time` | 0:  |
| `ffxi.get_player().merits` | `super_jump_recast` | 0:  |
| `ffxi.get_player().merits` | `tachi_shoha` | 0:  |
| `ffxi.get_player().merits` | `tame_recast` | 0:  |
| `ffxi.get_player().merits` | `third_eye_recast` | 0:  |
| `ffxi.get_player().merits` | `thunderstorm` | 0:  |
| `ffxi.get_player().merits` | `tomahawk` | 0:  |
| `ffxi.get_player().merits` | `tranquility` | 0:  |
| `ffxi.get_player().merits` | `trick_attack_recast` | 0:  |
| `ffxi.get_player().merits` | `triple_attack_rate` | 0:  |
| `ffxi.get_player().merits` | `troubadour` | 0:  |
| `ffxi.get_player().merits` | `unlimited_shot_recast` | 0:  |
| `ffxi.get_player().merits` | `upheaval` | 0:  |
| `ffxi.get_player().merits` | `vallation_effect` | 0:  |
| `ffxi.get_player().merits` | `ventriloquy` | 0:  |
| `ffxi.get_player().merits` | `warcry_recast` | 0:  |
| `ffxi.get_player().merits` | `warding_circle_recast` | 0:  |
| `ffxi.get_player().merits` | `warriors_charge` | 0:  |
| `ffxi.get_player().merits` | `water_magic_accuracy` | 0:  |
| `ffxi.get_player().merits` | `water_magic_potency` | 0:  |
| `ffxi.get_player().merits` | `weapon_bash_effect` | 0:  |
| `ffxi.get_player().merits` | `wind_blade` | 0:  |
| `ffxi.get_player().merits` | `wind_magic_accuracy` | 0:  |
| `ffxi.get_player().merits` | `wind_magic_potency` | 0:  |
| `ffxi.get_player().merits` | `winning_streak` | 0:  |
| `ffxi.get_player().merits` | `yonin_effect` | 0:  |
| `ffxi.get_player().merits` | `zanshin_attack_rate` | 0:  |
| `ffxi.get_player().skills` | `guard` | 0:  |
| `ffxi.get_player().skills` | `handbell` | 0:  |
| `ffxi.get_player().skills` | `ninjutsu` | 0:  |
| `ffxi.get_player().skills` | `stringed_instrument` | 0:  |
| `ffxi.get_player().skills` | `wind_instrument` | 0:  |
| `get_camera` | `aspect` | 0:  |
| `get_camera` | `fov_h` | 0:  |
| `get_camera` | `fov_v` | 0:  |
| `get_camera` | `matrix` | 0:  |
| `get_camera` | `matrix_inverse` | 0:  |
| `get_camera` | `orientation_w` | 0:  |
| `get_camera` | `orientation_x` | 0:  |
| `get_camera` | `orientation_y` | 0:  |
| `get_camera` | `orientation_z` | 0:  |
| `get_camera` | `projection_matrix` | 0:  |
| `get_camera` | `valid` | 0:  |
| `get_windower_settings` | `profile_name` | 0:  |

## By addon

- **answeringmachine**: event: chat message
- **autocontrol**: missing: windower.ffxi.reset_attachments; missing: windower.ffxi.set_attachment
- **autoinvite**: event: chat message
- **autojoin**: event: party invite
- **azureSets**: missing: windower.ffxi.remove_blue_magic_spell; missing: windower.ffxi.reset_blue_magic_spells; missing: windower.ffxi.set_blue_magic_spell
- **barfiller**: event: level down; event: level up
- **battlemod**: stub: windower.debug
- **bluguide**: missing: windower.ffxi.remove_blue_magic_spell; missing: windower.ffxi.set_blue_magic_spell
- **chatmon**: event: chat message; event: emote; event: examined; event: party invite
- **chatPorter**: event: linkshell change
- **ConsoleBG**: stub: windower.console.visible
- **craft**: missing: windower.ffxi.put_item
- **EmpyPopTracker**: event: add item; event: remove item
- **GearSwap**: stub: windower.debug
- **giltracker**: event: add item; event: remove item
- **InfoBar**: require: sqlite3
- **InfoReplacer**: missing: windower.ffxi.get_camera
- **invtracker**: event: add item; event: linkshell change; event: remove item
- **itemizer**: missing: windower.ffxi.move_item; missing: windower.ffxi.stack_items
- **latentchecker**: missing: windower.ffxi.put_item
- **linker**: missing: windower.net
- **Lookup**: event: add item
- **lottery**: missing: windower.ffxi.pass_item
- **organizer**: missing: windower.ffxi.get_mob_list; stub: windower.debug
- **respond**: event: chat message
- **switch_focus**: stub: windower.take_focus
- **Treasury**: missing: windower.ffxi.lot_item; missing: windower.ffxi.pass_item
- **whereisdi**: event: chat message; require: ssl.config; require: ssl.context; require: ssl.core; require: ssl.x509
- **XICamera**: require: _XICamera
- **XIPivot**: require: _XIPivot

Nothing lacking: allseeingeye, Anchor, AnnounceTarget, autoenterkey, autoex, AutoRA, Balloon, Bars, BattleStations, blist, BLUAlert, Bonanza, Botty, boxdestroyer, cancel, capetrader, cBlock, cellhelp, chars, ChatLink, checkparam, Clock, Currencies, Debuffed, diagnostics, digger, Dimmer, distance, DistancePlus, DressUp, dynamishelper, EasyNuke, enemybar, enternity, equipviewer, eval, FastCS, FastFollow, ffocolor, findAll, gametime, GMmode, hasteinfo, highlight, HomePoint, indinope, instaLS, JobChange, linuxfix, Logger, macrochanger, MobCompass, MountMuzzle, MountRoulette, MyCap, MyExp, MyHome, NoCampaignMusic, Nostrum, NPCDecoder, NyzulHelper, obiaway, ohShi, Omen, Overlay, packetviewer, parse, PetSchool, PetTP, plasmon, plugin_manager, pointwatch, porter, position_manager, Pouches, RAWR, reive, remember, renamer, Rhombus, roe, rolltracker, salvage2, SATACast, scoreboard, send, setbgm, SetTarget, shortcuts, shoutHelper, Silence, Skillchains, special, SpeedChecker, SpellBook, SpellCheck, statsearch, stna, stopwatch, StratHelper, Stubborn, SubTarget, superwarp, Tab, targetinfo, temps, Text, thtracker, Tickle, timestamp, tiptoes, TParty, translate, TreasurePool, TrueTargetLock, Trusts, TrustSupport, update, VisibleFavor, vwhl, xivbar, Yush, zonetimer
