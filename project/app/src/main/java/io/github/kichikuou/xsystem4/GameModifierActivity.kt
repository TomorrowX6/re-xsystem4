package io.github.kichikuou.xsystem4

import android.app.Activity
import android.app.AlertDialog
import android.os.Bundle
import android.text.InputType
import android.view.Gravity
import android.view.LayoutInflater
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

data class ModSaveSlotEntry(
    val file: File,
    val relativePath: String,
    val slotNum: Int,
    val tag: String,
    val sizeBytes: Long,
    val lastModified: Long
)

private class ModSaveSlotAdapter(
    private val activity: Activity,
    private var entries: List<ModSaveSlotEntry>
) : BaseAdapter() {

    private val dateFormat = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault())

    fun update(newEntries: List<ModSaveSlotEntry>) {
        entries = newEntries
        notifyDataSetChanged()
    }

    override fun getCount(): Int = entries.size
    override fun getItem(position: Int): ModSaveSlotEntry = entries[position]
    override fun getItemId(position: Int): Long = position.toLong()

    override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
        val view = convertView ?: LayoutInflater.from(activity)
            .inflate(android.R.layout.simple_list_item_2, parent, false)
        val entry = entries[position]
        val text1 = view.findViewById<TextView>(android.R.id.text1)
        val text2 = view.findViewById<TextView>(android.R.id.text2)

        val bakMark = if (GameModifierBridge.hasModBackup(entry.file)) " · 已备份" else ""
        text1.text = "${entry.relativePath}  [${entry.tag}$bakMark]"
        val sizeStr = String.format(Locale.US, "%.1f KB", entry.sizeBytes / 1024.0)
        val dateStr = dateFormat.format(Date(entry.lastModified))
        text2.text = "$sizeStr · $dateStr"
        return view
    }
}

class GameModifierActivity : Activity() {
    companion object {
        const val EXTRA_DIR_NAME = "EXTRA_DIR_NAME"
    }

    private var targets: MutableList<SaveTarget> = mutableListOf()
    private lateinit var currentTarget: SaveTarget
    private lateinit var adapter: ModSaveSlotAdapter
    private var currentEntries: List<ModSaveSlotEntry> = emptyList()

    private val isZh: Boolean
        get() = Locale.getDefault().language.startsWith("zh")
    private val isJa: Boolean
        get() = Locale.getDefault().language.startsWith("ja")

    private fun tr(zh: String, ja: String, en: String): String = when {
        isZh -> zh
        isJa -> ja
        else -> en
    }

    private fun dp(v: Int): Int =
        (v * resources.displayMetrics.density).toInt()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_game_modifier)
        actionBar?.setDisplayHomeAsUpEnabled(true)
        title = tr("游戏修改器", "ゲーム改造", "Game Modifier")

        findViewById<Button>(R.id.btn_mod_full_unlock).text = tr("全解锁存档", "全CG・実績解放", "Full Unlock")
        findViewById<Button>(R.id.btn_mod_global_save).text = tr("全局存档修改", "全体セーブ改造", "Global Save")
        findViewById<Button>(R.id.btn_mod_floating_cfg).text = tr("悬浮窗与锁定", "ゲーム内ロック", "In-Game Locks")

        adapter = ModSaveSlotAdapter(this, emptyList())
        val listView = findViewById<ListView>(R.id.mod_save_list)
        listView.adapter = adapter

        discoverSaveTargets(intent.getStringExtra(EXTRA_DIR_NAME))

        findViewById<View>(R.id.btn_mod_switch_dir).setOnClickListener { showSwitchTargetDialog() }
        findViewById<View>(R.id.btn_mod_full_unlock).setOnClickListener { showFullUnlockDialog() }
        findViewById<View>(R.id.btn_mod_global_save).setOnClickListener { showGlobalSaveMenuDialog() }
        findViewById<View>(R.id.btn_mod_floating_cfg).setOnClickListener { showFloatingLocksDialog() }
        findViewById<View>(R.id.btn_mod_refresh).setOnClickListener {
            refreshList()
            Toast.makeText(this, R.string.save_manager_refreshed, Toast.LENGTH_SHORT).show()
        }

        listView.setOnItemClickListener { _, _, pos, _ ->
            showLocalSaveMenuDialog(adapter.getItem(pos))
        }

        refreshList()
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId == android.R.id.home) {
            finish()
            return true
        }
        return super.onOptionsItemSelected(item)
    }

    private fun discoverSaveTargets(preferredDirName: String? = null) {
        val savesRoot = File(filesDir, "saves")
        savesRoot.mkdirs()

        val found = LinkedHashMap<String, SaveTarget>()
        val gameList = GameList(this)
        for (i in 0 until gameList.size) {
            val item = gameList[i]
            val dirName = item.path.name
            val saveDir = item.savedir ?: File(savesRoot, dirName)
            saveDir.mkdirs()
            found[dirName] = SaveTarget(
                displayName = "${item.name} ($dirName)",
                dirName = dirName,
                saveDir = saveDir,
                externalGameDir = item.path
            )
        }

        savesRoot.listFiles()?.filter { it.isDirectory && !it.name.startsWith('.') }?.forEach { dir ->
            if (!found.containsKey(dir.name)) {
                val extDir = getExternalFilesDir(null)?.let { File(it, dir.name) }?.takeIf { it.exists() }
                found[dir.name] = SaveTarget(
                    displayName = dir.name,
                    dirName = dir.name,
                    saveDir = dir,
                    externalGameDir = extDir
                )
            }
        }

        if (found.isEmpty()) {
            val defaultDir = File(savesRoot, "Rance10")
            defaultDir.mkdirs()
            val extDir = getExternalFilesDir(null)?.let { File(it, "Rance10") }
            found["Rance10"] = SaveTarget(
                displayName = getString(R.string.save_manager_default_dir_label),
                dirName = "Rance10",
                saveDir = defaultDir,
                externalGameDir = extDir
            )
        }

        targets = found.values.toMutableList()
        currentTarget = targets.firstOrNull { it.dirName == preferredDirName } ?: targets.first()
    }

    private fun refreshList() {
        val saveDir = currentTarget.saveDir
        saveDir.mkdirs()

        val regex = Regex("localsave(\\d+)\\.asd", RegexOption.IGNORE_CASE)
        val slots = mutableListOf<ModSaveSlotEntry>()
        saveDir.walkTopDown().forEach { f ->
            if (f.isFile) {
                val m = regex.matchEntire(f.name)
                if (m != null) {
                    val num = m.groupValues[1].toIntOrNull() ?: 0
                    val rel = f.relativeTo(saveDir).path.replace('\\', '/')
                    val tag = when {
                        num >= 5000 -> tr("节点存档", "拠点", "Checkpoint")
                        num >= 1000 -> tr("周回存档", "周回", "Clear")
                        num >= 100 -> tr("自动存档", "オート", "Auto")
                        else -> tr("普通存档", "通常", "Normal")
                    }
                    slots.add(ModSaveSlotEntry(f, rel, num, tag, f.length(), f.lastModified()))
                }
            }
        }
        slots.sortBy { it.slotNum }
        currentEntries = slots
        adapter.update(slots)

        val hasGlobal = File(saveDir, "GlobalSave.asd").isFile || File(saveDir, "Collection.asd").isFile
        val floatOn = GameModifierBridge.isFloatingEnabled(this)
        val lockCount = Integer.bitCount(GameModifierBridge.getSavedLockMask(this))

        findViewById<TextView>(R.id.mod_target_title).text =
            getString(R.string.save_manager_current_dir, currentTarget.displayName)
        findViewById<TextView>(R.id.mod_target_summary).text = tr(
            "进度存档 ${slots.size} 个 · 全局存档：${if (hasGlobal) "已有" else "无"} · 悬浮窗：${if (floatOn) "开启" else "关闭"} (锁定 $lockCount 项)",
            "セーブ ${slots.size} 個 · 全体：${if (hasGlobal) "あり" else "なし"} · ロック $lockCount 項目",
            "${slots.size} saves · Global: ${if (hasGlobal) "Yes" else "No"} · Locks: $lockCount"
        )

        val emptyView = findViewById<TextView>(R.id.mod_empty_view)
        val listView = findViewById<View>(R.id.mod_save_list)
        if (slots.isEmpty()) {
            emptyView.text = tr(
                "当前目录暂无进度存档 (LocalSave*.asd)。\n可使用上方“全解锁存档”、“全局存档修改”或“悬浮窗与锁定”。",
                "進行セーブ (LocalSave*.asd) がありません。\n上部のボタンから全解放やゲーム内ロックを設定できます。",
                "No progress saves (LocalSave*.asd) in this directory.\nUse the buttons above for Full Unlock, Global Save, or In-Game Locks."
            )
            emptyView.visibility = View.VISIBLE
            listView.visibility = View.GONE
        } else {
            emptyView.visibility = View.GONE
            listView.visibility = View.VISIBLE
        }
    }

    private fun showSwitchTargetDialog() {
        discoverSaveTargets(currentTarget.dirName)
        val labels = targets.map { target ->
            val count = target.saveDir.walkTopDown()
                .filter { it.isFile && it.name.endsWith(".asd", ignoreCase = true) }
                .count()
            getString(R.string.save_dir_item_count, target.displayName, count)
        }.toTypedArray()

        AlertDialog.Builder(this)
            .setTitle(R.string.save_dir_select_title)
            .setItems(labels) { _, which ->
                currentTarget = targets[which]
                refreshList()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    // =========================================================================
    // 1. Full Unlock Dialog
    // =========================================================================
    private fun showFullUnlockDialog() {
        val saveDir = currentTarget.saveDir
        val hasBak = GameModifierBridge.hasFullUnlockBackup(saveDir)
        val options = mutableListOf(
            tr("写入全CG / 全回想 / 全实绩解锁存档", "全CG・回想・実績解放セーブを適用", "Install Full Unlock Save")
        )
        if (hasBak) {
            options.add(tr("恢复解锁前备份", "解放前のバックアップを復元", "Restore Pre-Unlock Backup"))
        }

        AlertDialog.Builder(this)
            .setTitle(tr("全解锁存档", "全解放セーブ", "Full Unlock Save"))
            .setItems(options.toTypedArray()) { _, which ->
                if (which == 0) {
                    val count = GameModifierBridge.installFullUnlockSaves(this, saveDir)
                    refreshList()
                    Toast.makeText(
                        this,
                        tr("已写入 $count 个全解锁存档文件（仅覆盖全局与收集存档）", "$count 個の全解放ファイルを適用しました", "Installed $count unlock files"),
                        Toast.LENGTH_SHORT
                    ).show()
                } else if (which == 1 && hasBak) {
                    val restored = GameModifierBridge.restoreFullUnlockBackup(saveDir)
                    refreshList()
                    Toast.makeText(this, tr("已恢复 $restored 个备份文件", "$restored 個のファイルを復元しました", "Restored $restored files"), Toast.LENGTH_SHORT).show()
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    // =========================================================================
    // 2. Global Save Menu & Editor
    // =========================================================================
    private fun showGlobalSaveMenuDialog() {
        if (!GameModifierBridge.ensureLoaded()) {
            Toast.makeText(this, R.string.error, Toast.LENGTH_SHORT).show()
            return
        }
        val saveDir = currentTarget.saveDir
        val gFile = File(saveDir, "GlobalSave.asd")
        val cFile = File(saveDir, "Collection.asd")
        if (!gFile.isFile && !cFile.isFile) {
            AlertDialog.Builder(this)
                .setTitle(tr("未找到全局存档", "全体セーブなし", "No Global Save"))
                .setMessage(tr("当前目录尚无 GlobalSave.asd，是否先写入全解锁存档？", "先に全解放セーブを適用しますか？", "Install Full Unlock Save first?"))
                .setPositiveButton(R.string.ok) { _, _ ->
                    GameModifierBridge.installFullUnlockSaves(this, saveDir)
                    refreshList()
                    showGlobalSaveMenuDialog()
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
            return
        }

        val info = try {
            JSONObject(GameModifierBridge.readGlobalSaveSummaryForDir(saveDir))
        } catch (_: Throwable) {
            JSONObject()
        }
        val cp = info.optInt("totalClearPoint", 0)
        val medal = info.optInt("AutoBattleMedal", 0)
        val expUp = info.optInt("trophyExpBonus", 0)
        val hasBak = GameModifierBridge.hasModBackup(gFile) || GameModifierBridge.hasModBackup(cFile)

        val options = mutableListOf(
            tr("一键满CP / 满勋章 / 满实绩加成", "CP・メダル・実績ボーナス最大", "Max CP, Medals & Bonuses"),
            tr("自定义全局数值…", "数値を個別編集…", "Edit Global Values…")
        )
        if (hasBak) {
            options.add(tr("恢复修改前备份", "変更前のバックアップを復元", "Restore Backup"))
        }

        AlertDialog.Builder(this)
            .setTitle(tr("全局存档 (CP:$cp · 勋章:$medal · 经验+$expUp%)", "全体セーブ (CP:$cp · メダル:$medal)", "Global Save (CP:$cp)"))
            .setItems(options.toTypedArray()) { _, which ->
                when (which) {
                    0 -> {
                        val mod = JSONObject().apply {
                            put("totalClearPoint", 999)
                            put("clearBPointMax", 999)
                            put("TotalClearCount", 99)
                            put("Clear_A_Count", 99)
                            put("Clear_B_Count", 99)
                            put("Clear_C_Count", 99)
                            put("AutoBattleMedal", 999)
                            put("AchievementPoint", 999)
                            put("BestScore", 999999)
                            put("LeaderSmallestTurn", 1)
                            put("RealSmallestTurn", 1)
                            put("CollectionClearB", 1)
                            put("OpenMonsterpedia", 1)
                            put("TrophyComplete", 1)
                            put("CardComplete", 1)
                            put("leaderAtkRatio", 10.0)
                            put("trophyExpBonus", 9999)
                            put("trophyStatusMaxPer", 999)
                        }
                        applyGlobalSaveMod(mod)
                    }
                    1 -> showGlobalSaveCustomDialog(info)
                    2 -> {
                        var ok = false
                        if (GameModifierBridge.restoreModBackup(gFile)) ok = true
                        if (GameModifierBridge.restoreModBackup(cFile)) ok = true
                        if (ok) Toast.makeText(this, tr("已恢复备份", "復元しました", "Backup restored"), Toast.LENGTH_SHORT).show()
                    }
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showGlobalSaveCustomDialog(info: JSONObject) {
        val fields = listOf(
            Triple("totalClearPoint", tr("周回CP点", "周回CP", "Clear CP"), info.optInt("totalClearPoint", 0)),
            Triple("clearBPointMax", tr("第二部最高CP", "第2部最大CP", "Part 2 Max CP"), info.optInt("clearBPointMax", 0)),
            Triple("AutoBattleMedal", tr("自动战斗勋章", "自動戦闘メダル", "Auto Medals"), info.optInt("AutoBattleMedal", 0)),
            Triple("AchievementPoint", tr("实绩点数", "実績ポイント", "Achievement Pt"), info.optInt("AchievementPoint", 0)),
            Triple("TotalClearCount", tr("总通关次数", "総クリア回数", "Total Clears"), info.optInt("TotalClearCount", 0)),
            Triple("BestScore", tr("最高得分", "最高スコア", "Best Score"), info.optInt("BestScore", 0)),
            Triple("trophyExpBonus", tr("实绩经验加成%", "経験値ボーナス%", "EXP Bonus %"), info.optInt("trophyExpBonus", 0)),
            Triple("trophyStatusMaxPer", tr("属性上限加成%", "ステータス上限%", "Stat Cap %"), info.optInt("trophyStatusMaxPer", 0))
        )
        showNumberEditDialog(tr("自定义全局存档", "全体セーブ編集", "Edit Global Save"), fields) { mod ->
            applyGlobalSaveMod(mod)
        }
    }

    private fun applyGlobalSaveMod(mod: JSONObject) {
        val saveDir = currentTarget.saveDir
        val g1 = File(saveDir, "GlobalSave.asd")
        val c1 = File(saveDir, "Collection.asd")
        if (g1.isFile) GameModifierBridge.backupSaveFileBeforeMod(g1)
        if (c1.isFile) GameModifierBridge.backupSaveFileBeforeMod(c1)
        val ok = GameModifierBridge.writeGlobalSaveModForDir(saveDir, mod.toString())
        Toast.makeText(
            this,
            if (ok) tr("全局存档修改成功", "全体セーブを更新しました", "Global save updated")
            else tr("修改失败", "更新失敗", "Failed"),
            Toast.LENGTH_SHORT
        ).show()
    }

    // =========================================================================
    // 3. In-Game Floating Window & Locks Dialog
    // =========================================================================
    private fun showFloatingLocksDialog() {
        val lockDefs = listOf(
            0 to tr("游戏内显示“修改”悬浮按钮", "ゲーム内に改造ボタンを表示", "Show In-Game Floating Button"),
            GameModifierBridge.LOCK_PARTY_MAX_HP to tr("锁定我方满生命", "味方HP最大ロック", "Lock Party Max HP"),
            GameModifierBridge.LOCK_INFINITE_AP to tr("锁定无限AP (99)", "AP 99ロック", "Lock Infinite AP (99)"),
            GameModifierBridge.LOCK_BATTLE_COMBO to tr("锁定999连击加成", "999コンボロック", "Lock 999 Combo"),
            GameModifierBridge.LOCK_BATTLE_ROUND_1 to tr("锁定战斗回合为1", "戦闘ラウンド1固定", "Lock Round to 1"),
            GameModifierBridge.LOCK_ONE_HIT_KILL to tr("敌方1血秒杀", "敵HP1（一撃撃破）", "One-Hit Kill (Enemy HP=1)"),
            GameModifierBridge.LOCK_ENEMY_ZERO_ATK to tr("敌方0攻击0防御", "敵の攻防0化", "Enemy 0 ATK/DEF"),
            GameModifierBridge.LOCK_LEADER_BOOST_CRIT to tr("全员蓄力必暴连击", "全員ブースト・必殺", "All Leaders Boost & Crit"),
            GameModifierBridge.LOCK_INFINITE_SKILL_USE to tr("技能无限使用次数", "スキル回数無限", "Infinite Skill Uses"),
            GameModifierBridge.LOCK_INFINITE_ZIPPO to tr("无限悔棋打火机", "やり直しジッポ無限", "Infinite Retry Zippo"),
            GameModifierBridge.LOCK_MAX_RESOURCES to tr("资源不低于999", "チケット・勲章999維持", "Keep Resources >= 999")
        )

        var floatEnabled = GameModifierBridge.isFloatingEnabled(this)
        var lockMask = GameModifierBridge.getSavedLockMask(this)

        val labels = lockDefs.map { it.second }.toTypedArray()
        val checked = BooleanArray(lockDefs.size) { idx ->
            val bit = lockDefs[idx].first
            if (bit == 0) floatEnabled else (lockMask and bit) != 0
        }

        AlertDialog.Builder(this)
            .setTitle(tr("悬浮窗与实时锁定", "ゲーム内ロック設定", "In-Game Floating & Locks"))
            .setMultiChoiceItems(labels, checked) { _, which, isChecked ->
                val bit = lockDefs[which].first
                if (bit == 0) {
                    floatEnabled = isChecked
                } else {
                    lockMask = if (isChecked) (lockMask or bit) else (lockMask and bit.inv())
                }
            }
            .setPositiveButton(R.string.ok) { _, _ ->
                GameModifierBridge.setFloatingEnabled(this, floatEnabled)
                GameModifierBridge.setSavedLockMask(this, lockMask)
                refreshList()
                Toast.makeText(this, tr("设置已保存", "設定を保存しました", "Settings saved"), Toast.LENGTH_SHORT).show()
            }
            .setNeutralButton(tr("全部关闭", "すべて解除", "Clear All")) { _, _ ->
                GameModifierBridge.setSavedLockMask(this, 0)
                refreshList()
                Toast.makeText(this, tr("已关闭全部锁定", "すべてのロックを解除しました", "All locks cleared"), Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    // =========================================================================
    // 4. LocalSave*.asd Slot Menu & Editors
    // =========================================================================
    private fun showLocalSaveMenuDialog(entry: ModSaveSlotEntry) {
        if (!GameModifierBridge.ensureLoaded()) {
            Toast.makeText(this, R.string.error, Toast.LENGTH_SHORT).show()
            return
        }
        val info = try {
            JSONObject(GameModifierBridge.nativeReadLocalSaveSummary(entry.file.absolutePath))
        } catch (_: Throwable) {
            JSONObject()
        }
        if (!info.optBoolean("ok", false)) {
            Toast.makeText(this, tr("无法解析该存档文件", "セーブを解析できません", "Cannot parse save file"), Toast.LENGTH_SHORT).show()
            return
        }

        val turn = info.optInt("turn", 0)
        val food = info.optInt("foodTicket", 0)
        val honor = info.optInt("honor", 0)
        val hp = info.optInt("partyHp", 0)
        val maxHp = info.optInt("partyMaxHp", 0)
        val hasBak = GameModifierBridge.hasModBackup(entry.file)

        val options = mutableListOf(
            tr("一键满资源与满属性", "リソース・部隊ステータス最大", "One-Click Max Resources & Stats"),
            tr("一键全卡牌99叠卡与全员5星", "全カード99枚＋全キャラ★5", "One-Click All Cards x99 & 5★"),
            tr("一键四国满兵力与人类零伤亡", "四国兵力最大＋人類被害ゼロ", "One-Click Max Armies & 0 Casualties"),
            tr("修改资源与回合进度…", "リソース・ターン編集…", "Edit Resources & Turn…"),
            tr("修改部队生命与属性加成…", "部隊HP・ステータス倍率編集…", "Edit Party HP & Multipliers…"),
            tr("修改四国兵力与人类人口…", "四国兵力・人口編集…", "Edit World & Armies…"),
            tr("修改卡牌叠卡与角色星级…", "カード枚数・キャラ★編集…", "Edit Cards & Characters…")
        )
        if (hasBak) {
            options.add(tr("恢复修改前备份", "変更前のバックアップを復元", "Restore Pre-Mod Backup"))
        }

        AlertDialog.Builder(this)
            .setTitle("${entry.file.name} (${tr("回合", "T", "Turn")} $turn · ${tr("食券", "食券", "Food")} $food · ${tr("勋章", "勲章", "Medal")} $honor · HP $hp/$maxHp)")
            .setItems(options.toTypedArray()) { _, which ->
                when (which) {
                    0 -> {
                        val mod = JSONObject().apply {
                            put("difficulty", 0)
                            put("foodTicket", 999)
                            put("honor", 999)
                            put("friendPoint", 999)
                            put("totalExp", 99999999)
                            put("partyRank", 999)
                            put("treasureNotGetCount", 99)
                            put("itemStar", 99)
                            put("isUsedHannyZippo", 0)
                            put("hannyZippo", 99)
                            put("partyBonusMaxPoint", 99)
                            put("partyHp", 999999)
                            put("partyMaxHp", 999999)
                            put("partyAvailCount", 9)
                            put("partyIsFixStatus", 1)
                            put("partyAtk", 99999)
                            put("allStatusPer", 999)
                        }
                        applyLocalSaveMod(entry, mod)
                    }
                    1 -> {
                        val mod = JSONObject().apply {
                            put("setCardCount", 99)
                            put("cardAtkBonus", 999)
                            put("cardHpBonus", 999)
                            put("setCharStar", 5)
                            put("setCharExp", 999999)
                        }
                        applyLocalSaveMod(entry, mod)
                    }
                    2 -> {
                        val mod = JSONObject().apply {
                            put("humanCount", 350000)
                            put("humanDead", 0)
                            put("humanAlivePer", 100)
                            put("humanDeadPer", 0)
                            put("curseTurn", 0)
                            put("rareLuck", 999)
                            put("seaPlayerArmy", 99999)
                            put("seaEnemyArmy", 0)
                            put("leazasState", 0)
                            put("helmanState", 0)
                            put("zethState", 0)
                            put("freeCitiesState", 0)
                            put("leazasPArmy", 99999)
                            put("helmanPArmy", 99999)
                            put("zethPArmy", 99999)
                            put("freeCitiesPArmy", 99999)
                            put("leazasEArmy", 100)
                            put("helmanEArmy", 100)
                            put("zethEArmy", 100)
                            put("freeCitiesEArmy", 100)
                            put("subRemnant", 99999)
                            put("subDwarf", 99999)
                            put("subGeneral", 99999)
                            put("subHanny", 99999)
                        }
                        applyLocalSaveMod(entry, mod)
                    }
                    3 -> showLocalSaveResourcesDialog(entry, info)
                    4 -> showLocalSavePartyDialog(entry, info)
                    5 -> showLocalSaveWorldDialog(entry, info)
                    6 -> showLocalSaveCardsDialog(entry)
                    7 -> {
                        if (GameModifierBridge.restoreModBackup(entry.file)) {
                            refreshList()
                            Toast.makeText(this, tr("已恢复备份", "バックアップを復元しました", "Backup restored"), Toast.LENGTH_SHORT).show()
                        }
                    }
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showLocalSaveResourcesDialog(entry: ModSaveSlotEntry, info: JSONObject) {
        val fields = listOf(
            Triple("turn", tr("当前回合", "現在ターン", "Current Turn"), info.optInt("turn", 1)),
            Triple("chapter", tr("当前章节", "現在チャプター", "Chapter"), info.optInt("chapter", 0)),
            Triple("difficulty", tr("难度 (0易~10难)", "難易度 (0~10)", "Difficulty (0~10)"), info.optInt("difficulty", 1)),
            Triple("foodTicket", tr("食券数量", "食券", "Food Tickets"), info.optInt("foodTicket", 0)),
            Triple("honor", tr("勋章数量", "勲章", "Medals"), info.optInt("honor", 0)),
            Triple("friendPoint", tr("友情点数", "友好ポイント", "Friend Points"), info.optInt("friendPoint", 0)),
            Triple("totalExp", tr("累计经验值", "累計経験値", "Total EXP"), info.optInt("totalExp", 0)),
            Triple("partyRank", tr("部队等级", "部隊ランク", "Party Rank"), info.optInt("partyRank", 1)),
            Triple("itemStar", tr("装备星级加成", "装備★加算", "Item Star Bonus"), info.optInt("itemStar", 0)),
            Triple("treasureNotGetCount", tr("稀有保底计数", "レア宝箱カウント", "Rare Pity Count"), info.optInt("treasureNotGetCount", 0)),
            Triple("hannyZippo", tr("悔棋打火机次数", "ジッポ回数", "Zippo Count"), info.optInt("hannyZippo", 0))
        )
        showNumberEditDialog("${entry.file.name} - ${tr("资源与进度", "リソース・進行", "Resources")}", fields) { mod ->
            applyLocalSaveMod(entry, mod)
        }
    }

    private fun showLocalSavePartyDialog(entry: ModSaveSlotEntry, info: JSONObject) {
        val fields = listOf(
            Triple("partyHp", tr("部队当前生命", "現在HP", "Party HP"), info.optInt("partyHp", 1000)),
            Triple("partyMaxHp", tr("部队最大生命", "最大HP", "Party Max HP"), info.optInt("partyMaxHp", 1000)),
            Triple("partyAvailCount", tr("每回合出击人数", "出撃枠数", "Deploy Slots"), info.optInt("partyAvailCount", 4)),
            Triple("partyIsFixStatus", tr("锁定属性不被重算(0/1)", "ステータス固定(0/1)", "Lock Stats (0/1)"), info.optInt("partyIsFixStatus", 0)),
            Triple("partyAtk", tr("部队总攻击力", "総攻撃力", "Total ATK"), info.optInt("partyAtk", 500)),
            Triple("atkPerPhys", tr("物理攻击加成%", "物理攻撃%", "Phys ATK %"), info.optInt("atkPerPhys", 0)),
            Triple("atkPerMag", tr("魔法攻击加成%", "魔法攻撃%", "Mag ATK %"), info.optInt("atkPerMag", 0)),
            Triple("defPerPhys", tr("物理防御加成%", "物理防御%", "Phys DEF %"), info.optInt("defPerPhys", 0)),
            Triple("defPerMag", tr("魔法防御加成%", "魔法防御%", "Mag DEF %"), info.optInt("defPerMag", 0)),
            Triple("perCrit", tr("暴击率加成%", "クリティカル%", "Crit %"), info.optInt("perCrit", 0)),
            Triple("perMulti", tr("连击率加成%", "連撃率%", "Multi-Hit %"), info.optInt("perMulti", 0)),
            Triple("perExpUp", tr("经验获取加成%", "経験値%", "EXP Bonus %"), info.optInt("perExpUp", 0)),
            Triple("perFindTreasure", tr("寻宝发现率%", "宝箱発見率%", "Treasure %"), info.optInt("perFindTreasure", 0))
        )
        showNumberEditDialog("${entry.file.name} - ${tr("部队与属性", "部隊・ステータス", "Party & Stats")}", fields) { mod ->
            applyLocalSaveMod(entry, mod)
        }
    }

    private fun showLocalSaveWorldDialog(entry: ModSaveSlotEntry, info: JSONObject) {
        val fields = listOf(
            Triple("humanCount", tr("人类存活人口", "人類生存人口", "Human Alive"), info.optInt("humanCount", 300000)),
            Triple("humanDead", tr("人类死亡人口", "人類死亡人口", "Human Dead"), info.optInt("humanDead", 0)),
            Triple("curseTurn", tr("大诅咒回合数", "大呪いターン", "Curse Turns"), info.optInt("curseTurn", 0)),
            Triple("rareLuck", tr("稀有运加成", "レア運", "Rare Luck"), info.optInt("rareLuck", 0)),
            Triple("seaPlayerArmy", tr("兰斯船团兵力", "ランス船団兵力", "Sea Fleet Army"), info.optInt("seaPlayerArmy", 0)),
            Triple("leazasPArmy", tr("利萨斯我军兵力", "リーザス味方兵力", "Leazas Player Army"), info.optInt("leazasPArmy", 300)),
            Triple("helmanPArmy", tr("赫尔曼我军兵力", "ヘルマン味方兵力", "Helman Player Army"), info.optInt("helmanPArmy", 300)),
            Triple("zethPArmy", tr("赛斯我军兵力", "ゼス味方兵力", "Zeth Player Army"), info.optInt("zethPArmy", 300)),
            Triple("freeCitiesPArmy", tr("自由都市我军兵力", "自由都市味方兵力", "Free Cities Player Army"), info.optInt("freeCitiesPArmy", 300)),
            Triple("leazasEArmy", tr("利萨斯魔军兵力", "リーザス魔軍兵力", "Leazas Enemy Army"), info.optInt("leazasEArmy", 500)),
            Triple("helmanEArmy", tr("赫尔曼魔军兵力", "ヘルマン魔軍兵力", "Helman Enemy Army"), info.optInt("helmanEArmy", 500)),
            Triple("zethEArmy", tr("赛斯魔军兵力", "ゼス魔軍兵力", "Zeth Enemy Army"), info.optInt("zethEArmy", 500)),
            Triple("freeCitiesEArmy", tr("自由都市魔军兵力", "自由都市魔軍兵力", "Free Cities Enemy Army"), info.optInt("freeCitiesEArmy", 500))
        )
        showNumberEditDialog("${entry.file.name} - ${tr("四国与人口", "四国・人口", "World & Armies")}", fields) { mod ->
            applyLocalSaveMod(entry, mod)
        }
    }

    private fun showLocalSaveCardsDialog(entry: ModSaveSlotEntry) {
        val fields = listOf(
            Triple("setCardCount", tr("全卡牌叠卡设为N张", "全カード枚数指定", "Set All Cards Count"), 99),
            Triple("cardAtkBonus", tr("全卡牌攻击加成", "全カード攻撃加算", "All Cards ATK Bonus"), 999),
            Triple("cardHpBonus", tr("全卡牌生命加成", "全カードHP加算", "All Cards HP Bonus"), 999),
            Triple("setCharStar", tr("全角色星级 (1~5)", "全キャラ★ (1~5)", "All Chars Star (1~5)"), 5),
            Triple("setCharExp", tr("全角色经验值", "全キャラ経験値", "All Chars EXP"), 999999)
        )
        showNumberEditDialog("${entry.file.name} - ${tr("卡牌与角色", "カード・キャラ", "Cards & Chars")}", fields) { mod ->
            applyLocalSaveMod(entry, mod)
        }
    }

    private fun applyLocalSaveMod(entry: ModSaveSlotEntry, mod: JSONObject) {
        GameModifierBridge.backupSaveFileBeforeMod(entry.file)
        val ok = GameModifierBridge.nativeWriteLocalSaveMod(entry.file.absolutePath, mod.toString())
        refreshList()
        Toast.makeText(
            this,
            if (ok) tr("已修改 ${entry.file.name}", "${entry.file.name} を更新しました", "Updated ${entry.file.name}")
            else tr("修改失败", "更新失敗", "Failed"),
            Toast.LENGTH_SHORT
        ).show()
    }

    // =========================================================================
    // Helper for Compact Native Number Edit Dialog
    // =========================================================================
    private fun showNumberEditDialog(
        title: String,
        fields: List<Triple<String, String, Int>>,
        onConfirm: (JSONObject) -> Unit
    ) {
        val scroll = ScrollView(this)
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(12), dp(20), dp(8))
        }
        val inputs = LinkedHashMap<String, EditText>()
        for ((key, label, defVal) in fields) {
            val row = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(0, dp(4), 0, dp(4))
            }
            val tv = TextView(this).apply {
                text = label
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.2f)
            }
            val et = EditText(this).apply {
                inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED
                setText(defVal.toString())
                setSingleLine(true)
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.8f)
            }
            inputs[key] = et
            row.addView(tv)
            row.addView(et)
            container.addView(row)
        }
        scroll.addView(container)

        AlertDialog.Builder(this)
            .setTitle(title)
            .setView(scroll)
            .setPositiveButton(R.string.ok) { _, _ ->
                val mod = JSONObject()
                for ((key, et) in inputs) {
                    val v = et.text.toString().trim().toIntOrNull()
                    if (v != null) mod.put(key, v)
                }
                onConfirm(mod)
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }
}
