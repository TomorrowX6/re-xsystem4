package io.github.kichikuou.xsystem4

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class GameModifierActivity : Activity() {
    companion object {
        const val EXTRA_DIR_NAME = "EXTRA_DIR_NAME"
    }

    private data class SaveTargetItem(
        val displayName: String,
        val dirName: String,
        val saveDir: File
    )

    private data class LocalSaveSlot(
        val file: File,
        val slotNumber: Int,
        val summary: JSONObject?
    )

    private var targets: MutableList<SaveTargetItem> = mutableListOf()
    private lateinit var currentTarget: SaveTargetItem
    private lateinit var targetHeaderTitle: TextView
    private lateinit var targetHeaderSubtitle: TextView

    private val tabButtons = mutableListOf<Button>()
    private val tabContainers = mutableListOf<View>()
    private var currentTab = 0

    // Tab 1 views
    private lateinit var globalStatusText: TextView
    private lateinit var btnRestoreUnlockBackup: Button
    private lateinit var editGlobalPlayCount: EditText
    private lateinit var editGlobalItemStar: EditText
    private lateinit var editGlobalLeaderAtkRatio: EditText
    private lateinit var editGlobalExpUp: EditText
    private lateinit var editGlobalStatusMax: EditText
    private lateinit var chkGlobalMaxCharRank: CheckBox
    private lateinit var chkGlobalMaxTakeOver: CheckBox

    // Tab 2 views
    private var localSlots: List<LocalSaveSlot> = emptyList()
    private var selectedSlot: LocalSaveSlot? = null
    private lateinit var slotTitleText: TextView
    private lateinit var slotCommentText: TextView
    private lateinit var btnRestoreSlotBackup: Button
    private val localFields = LinkedHashMap<String, EditText>()
    private lateinit var editBatchSetCardCount: EditText
    private lateinit var editBatchAddCardCount: EditText
    private lateinit var editBatchBoostCardAtk: EditText
    private lateinit var editBatchBoostCardHp: EditText
    private lateinit var editBatchSetCharStar: EditText
    private lateinit var editBatchSetCharExp: EditText

    // Tab 3 views
    private lateinit var switchFloatingWindow: Switch
    private val lockCheckBoxes = LinkedHashMap<Int, CheckBox>()

    private val isZh: Boolean
        get() = Locale.getDefault().language.startsWith("zh")
    private val isJa: Boolean
        get() = Locale.getDefault().language.startsWith("ja")

    private fun tr(zh: String, ja: String, en: String): String = when {
        isZh -> zh
        isJa -> ja
        else -> en
    }

    private fun dp(value: Int): Int =
        (value * resources.displayMetrics.density).toInt()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        actionBar?.setDisplayHomeAsUpEnabled(true)
        title = getString(R.string.action_game_modifier)

        GameModifierBridge.ensureLoaded()
        discoverSaveTargets(intent.getStringExtra(EXTRA_DIR_NAME))

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.parseColor("#12151C"))
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        }

        root.addView(buildTopHeader())
        root.addView(buildTabBar())

        val contentFrame = ScrollView(this).apply {
            isFillViewport = true
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1f
            )
        }
        val pagesHolder = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(10), dp(12), dp(24))
        }

        val tab1 = buildTab1UnlockAndGlobal()
        val tab2 = buildTab2LocalSaveEditor()
        val tab3 = buildTab3FloatingAndLocks()

        tabContainers.add(tab1)
        tabContainers.add(tab2)
        tabContainers.add(tab3)

        pagesHolder.addView(tab1)
        pagesHolder.addView(tab2)
        pagesHolder.addView(tab3)
        contentFrame.addView(pagesHolder)
        root.addView(contentFrame)

        setContentView(root)
        selectTab(0)
        refreshAllForCurrentTarget()
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

        val found = LinkedHashMap<String, SaveTargetItem>()
        val gameList = GameList(this)
        for (i in 0 until gameList.size) {
            val item = gameList[i]
            val dirName = item.path.name
            val saveDir = item.savedir ?: File(savesRoot, dirName)
            saveDir.mkdirs()
            found[dirName] = SaveTargetItem(
                displayName = "${item.name} ($dirName)",
                dirName = dirName,
                saveDir = saveDir
            )
        }

        savesRoot.listFiles()?.filter { it.isDirectory && !it.name.startsWith('.') }?.forEach { dir ->
            if (!found.containsKey(dir.name)) {
                found[dir.name] = SaveTargetItem(
                    displayName = dir.name,
                    dirName = dir.name,
                    saveDir = dir
                )
            }
        }

        if (found.isEmpty()) {
            val defaultDir = File(savesRoot, "Rance10")
            defaultDir.mkdirs()
            found["Rance10"] = SaveTargetItem(
                displayName = getString(R.string.save_manager_default_dir_label),
                dirName = "Rance10",
                saveDir = defaultDir
            )
        }

        targets = found.values.toMutableList()
        currentTarget = targets.firstOrNull { it.dirName == preferredDirName } ?: targets.first()
    }

    private fun buildTopHeader(): View {
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(Color.parseColor("#1C222E"))
            setPadding(dp(14), dp(10), dp(14), dp(10))
        }

        val textCol = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        targetHeaderTitle = TextView(this).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            setTypeface(typeface, Typeface.BOLD)
        }
        targetHeaderSubtitle = TextView(this).apply {
            setTextColor(Color.parseColor("#9BA7B8"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
        }
        textCol.addView(targetHeaderTitle)
        textCol.addView(targetHeaderSubtitle)

        val switchBtn = makeStyledButton(
            tr("切换存档目录", "対象フォルダ切替", "Switch Save Dir"),
            "#2E5AAC"
        ) {
            showSwitchTargetDialog()
        }

        bar.addView(textCol)
        bar.addView(switchBtn)
        return bar
    }

    private fun buildTabBar(): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Color.parseColor("#171B24"))
            setPadding(dp(8), dp(6), dp(8), dp(6))
        }
        val titles = listOf(
            tr("🏆 全解锁与全局", "🏆 全解放・全体", "🏆 Full Unlock & Global"),
            tr("📝 游戏外存档修改", "📝 セーブデータ編集", "📝 Save Slot Editor"),
            tr("⚙ 悬浮窗与实时修改", "⚙ オーバーレイ・常時", "⚙ Floating & Live Mod")
        )
        titles.forEachIndexed { idx, title ->
            val btn = Button(this).apply {
                text = title
                isAllCaps = false
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                setPadding(dp(6), dp(8), dp(6), dp(8))
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                    setMargins(dp(4), 0, dp(4), 0)
                }
                setOnClickListener { selectTab(idx) }
            }
            tabButtons.add(btn)
            row.addView(btn)
        }
        return row
    }

    private fun selectTab(index: Int) {
        currentTab = index
        tabContainers.forEachIndexed { idx, view ->
            view.visibility = if (idx == index) View.VISIBLE else View.GONE
        }
        tabButtons.forEachIndexed { idx, btn ->
            val bg = GradientDrawable().apply {
                cornerRadius = dp(8).toFloat()
                setColor(if (idx == index) Color.parseColor("#D97706") else Color.parseColor("#262C3A"))
            }
            btn.background = bg
            btn.setTextColor(if (idx == index) Color.WHITE else Color.parseColor("#B0BAC9"))
            btn.setTypeface(null, if (idx == index) Typeface.BOLD else Typeface.NORMAL)
        }
    }

    private fun buildTab1UnlockAndGlobal(): View {
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }

        // Card 1: One-Click 100% Full Unlock
        val card1 = makeCardContainer()
        card1.addView(makeSectionTitle(
            tr(
                "⚡ 一键导入 100% 完美全解锁存档",
                "⚡ ワンクリック 100% 完全解放セーブ導入",
                "⚡ One-Click 100% Full Unlock Save"
            )
        ))
        card1.addView(makeBodyText(
            tr(
                "• 标题界面直接开启：【第二部】、【挑战模式】、【联合部队】全部入口\n" +
                    "• 全部 109 项实绩（Trophies）100% 达成，实绩经验与属性上限加成全满\n" +
                    "• 全 CG 画廊、全 BGM 鉴赏、全事件回想、多周目继承点数与继承卡牌全开\n" +
                    "• 仅写入全局与收集存档（Collection / GlobalSave / AFCGMode / AFBGMMode），绝不覆盖您的任何 LocalSave 游戏进度存档，并自动创建备份。",
                "• タイトル画面で「第二部」「挑戦モード」「連合部隊」を即時解放\n" +
                    "• 全109実績（トロフィー）100%達成、全CG・BGM・回想・周回引継ぎ要素を完全解放\n" +
                    "• 進行セーブ（LocalSave*.asd）は一切上書きせず、元データの自動バックアップを作成します。",
                "• Instantly unlocks Part 2, Challenge Mode, and Allied Forces on the title screen\n" +
                    "• Unlocks all 109 Trophies, 100% CG Gallery, BGM Mode, Event Replays, and NG+ bonuses\n" +
                    "• Only updates GlobalSave/Collection/AF Mode files without touching your LocalSave progress slots (auto-backups existing files)."
            )
        ))

        val btnInstallUnlock = makeStyledButton(
            tr("⚡ 一键安装 100% 全解锁存档", "⚡ 100% 全解放セーブを適用", "⚡ Install 100% Full Unlock Save"),
            "#D97706"
        ) {
            AlertDialog.Builder(this)
                .setTitle(tr("确认安装 100% 全解锁存档", "100% 全解放セーブの適用確認", "Confirm 100% Full Unlock"))
                .setMessage(
                    tr(
                        "即将向「${currentTarget.displayName}」写入 100% 全解锁全局与收集存档（原文件将自动备份，不影响进度存档）。是否继续？",
                        "「${currentTarget.displayName}」に100%全解放セーブを適用します（元の全体セーブは自動バックアップされます）。よろしいですか？",
                        "Install 100% Full Unlock Global & Collection saves into \"${currentTarget.displayName}\"? Existing files will be backed up automatically."
                    )
                )
                .setPositiveButton(R.string.ok) { _, _ ->
                    try {
                        val count = GameModifierBridge.installFullUnlockSaves(this, currentTarget.saveDir)
                        refreshAllForCurrentTarget()
                        Toast.makeText(
                            this,
                            tr(
                                "已成功安装 $count 个全解锁存档文件！启动游戏即可体验全部模式。",
                                "$count 個の全解放セーブを適用しました！",
                                "Installed $count full-unlock save files!"
                            ),
                            Toast.LENGTH_LONG
                        ).show()
                    } catch (e: Exception) {
                        Toast.makeText(this, "${getString(R.string.error)}: ${e.message}", Toast.LENGTH_LONG).show()
                    }
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(10)
            }
        }
        card1.addView(btnInstallUnlock)

        btnRestoreUnlockBackup = makeStyledButton(
            tr("↩ 恢复全解锁前的原版全局存档备份", "↩ 適用前の全体セーブバックアップを復元", "↩ Restore Pre-Unlock Global Backup"),
            "#374151"
        ) {
            val restored = GameModifierBridge.restoreFullUnlockBackup(currentTarget.saveDir)
            refreshAllForCurrentTarget()
            Toast.makeText(
                this,
                tr("已恢复 $restored 个原版全局存档文件", "$restored 個のファイルを復元しました", "Restored $restored backup files"),
                Toast.LENGTH_SHORT
            ).show()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(8)
            }
        }
        card1.addView(btnRestoreUnlockBackup)
        container.addView(card1)

        // Card 2: GlobalSave.asd & Collection.asd custom editor
        val card2 = makeCardContainer()
        card2.addView(makeSectionTitle(
            tr(
                "🌐 全局与收集存档自定义修改 (GlobalSave / Collection)",
                "🌐 全体・コレクションセーブ個別編集",
                "🌐 GlobalSave & Collection Editor"
            )
        ))
        globalStatusText = makeBodyText("")
        card2.addView(globalStatusText)

        editGlobalPlayCount = addLabeledNumberField(
            card2,
            tr("周目通关次数 (m_gamePlayCount)", "周回クリア回数 (m_gamePlayCount)", "Playthrough Count (m_gamePlayCount)"),
            "50"
        )
        editGlobalItemStar = addLabeledNumberField(
            card2,
            tr("全局最高装备星级 (m_itemStarRank)", "全体最高アイテム星ランク (m_itemStarRank)", "Global Max Item Star (m_itemStarRank)"),
            "20"
        )
        editGlobalLeaderAtkRatio = addLabeledNumberField(
            card2,
            tr("实绩全员攻击倍率% (<LeaderAtkRatio>)", "実績リーダー攻撃倍率% (<LeaderAtkRatio>)", "Trophy Leader ATK Ratio% (<LeaderAtkRatio>)"),
            "500"
        )
        editGlobalExpUp = addLabeledNumberField(
            card2,
            tr("实绩经验值加成% (m_PerExpUp)", "実績経験値ボーナス% (m_PerExpUp)", "Trophy EXP Bonus% (m_PerExpUp)"),
            "500"
        )
        editGlobalStatusMax = addLabeledNumberField(
            card2,
            tr("实绩属性上限突破% (StatusMaxPer)", "実績ステータス上限突破% (StatusMaxPer)", "Trophy Status Cap Bonus% (StatusMaxPer)"),
            "200"
        )

        chkGlobalMaxCharRank = CheckBox(this).apply {
            text = tr(
                "一键将【联合部队】全部角色阶级与事件阶段升至满级 (Rank 10)",
                "「連合部隊」全キャラのランク＆イベント段階を最大にする",
                "Max all Allied Forces Character Ranks & Event Stages"
            )
            setTextColor(Color.WHITE)
            isChecked = true
        }
        chkGlobalMaxTakeOver = CheckBox(this).apply {
            text = tr(
                "一键将全部已解锁【多周目继承卡牌】叠加数量升至 99 张",
                "周回引継ぎカードの所持枚数をすべて 99 枚にする",
                "Set all unlocked NG+ Take-Over Card stack counts to 99"
            )
            setTextColor(Color.WHITE)
            isChecked = true
        }
        card2.addView(chkGlobalMaxCharRank)
        card2.addView(chkGlobalMaxTakeOver)

        val btnSaveGlobalMod = makeStyledButton(
            tr("💾 保存全局与收集存档修改", "💾 全体・コレクション変更を保存", "💾 Save Global & Collection Changes"),
            "#10B981"
        ) {
            applyGlobalSaveModification()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(10)
            }
        }
        card2.addView(btnSaveGlobalMod)
        container.addView(card2)

        return container
    }

    private fun buildTab2LocalSaveEditor(): View {
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }

        // Top Slot Selector Card
        val slotCard = makeCardContainer()
        slotCard.addView(makeSectionTitle(
            tr("📂 进度存档槽位选择 (LocalSave*.asd)", "📂 進行セーブスロット選択 (LocalSave*.asd)", "📂 LocalSave Slot Selection")
        ))
        slotTitleText = TextView(this).apply {
            setTextColor(Color.parseColor("#FBBF24"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            setTypeface(typeface, Typeface.BOLD)
        }
        slotCommentText = TextView(this).apply {
            setTextColor(Color.parseColor("#D1D5DB"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            setPadding(0, dp(4), 0, dp(8))
        }
        slotCard.addView(slotTitleText)
        slotCard.addView(slotCommentText)

        val slotBtnRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
        }
        val btnChooseSlot = makeStyledButton(
            tr("📂 切换存档槽位", "📂 スロット切替", "📂 Select Slot"),
            "#2563EB"
        ) {
            showSelectLocalSlotDialog()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                rightMargin = dp(6)
            }
        }
        val btnReloadSlot = makeStyledButton(
            tr("🔄 重新读取", "🔄 再読込", "🔄 Reload"),
            "#4B5563"
        ) {
            reloadLocalSlots(keepCurrent = true)
            Toast.makeText(this, tr("已重新读取存档数据", "再読み込みしました", "Reloaded save data"), Toast.LENGTH_SHORT).show()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        slotBtnRow.addView(btnChooseSlot)
        slotBtnRow.addView(btnReloadSlot)
        slotCard.addView(slotBtnRow)

        val saveBtnRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(8)
            }
        }
        val btnSaveSlot = makeStyledButton(
            tr("💾 保存全部修改到当前存档", "💾 変更を現在のセーブに保存", "💾 Save All Edits to Slot"),
            "#10B981"
        ) {
            saveCurrentLocalSlotModifications()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.4f).apply {
                rightMargin = dp(6)
            }
        }
        btnRestoreSlotBackup = makeStyledButton(
            tr("↩ 恢复备份", "↩ バックアップ復元", "↩ Restore Bak"),
            "#374151"
        ) {
            val slot = selectedSlot ?: return@makeStyledButton
            if (GameModifierBridge.restoreModBackup(slot.file)) {
                reloadLocalSlots(keepCurrent = true)
                Toast.makeText(this, tr("已恢复修改前备份", "バックアップを復元しました", "Backup restored"), Toast.LENGTH_SHORT).show()
            }
        }.apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.8f)
        }
        saveBtnRow.addView(btnSaveSlot)
        saveBtnRow.addView(btnRestoreSlotBackup)
        slotCard.addView(saveBtnRow)
        container.addView(slotCard)

        // One-Click Presets Card
        val presetCard = makeCardContainer()
        presetCard.addView(makeSectionTitle(
            tr("⚡ 一键快捷神级预设 (填入下方表单，点击保存生效)", "⚡ ワンクリック最強プリセット", "⚡ One-Click Super Presets")
        ))
        val presetBtn1 = makeStyledButton(
            tr("💰 一键满资源 (食券/勋章/友情=9999, 经验=99999999, 装备星级=20)", "💰 リソース最大 (食券/勲章/友好=9999, EXP最大, 装備星20)", "💰 Max Resources, EXP & Item Star"),
            "#D97706"
        ) {
            setFieldText("foodTicket", "9999")
            setFieldText("honor", "9999")
            setFieldText("friendPoint", "9999")
            setFieldText("totalExp", "99999999")
            setFieldText("partyRank", "999")
            setFieldText("itemStar", "20")
            setFieldText("treasureNotGetCount", "0")
            setFieldText("partyBonusMaxPoint", "999")
            setFieldText("isUsedHannyZippo", "0")
            setFieldText("hannyZippo", "9")
            setFieldText("curseTurn", "0")
            setFieldText("rareLuck", "999")
            Toast.makeText(this, tr("已填入满资源预设，点击【保存全部修改】即可写入存档", "最大リソース値を入力しました。保存ボタンで適用されます", "Preset filled! Tap Save to write."), Toast.LENGTH_SHORT).show()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = dp(6)
            }
        }
        val presetBtn2 = makeStyledButton(
            tr("🛡 一键最强部队属性 (HP=9999999, 全16项攻防暴连倍率+500%, 锁定属性)", "🛡 最強部隊ステータス (HP=9999999, 全16種倍率+500%, 固定ON)", "🛡 Max Party HP & +500% All 16 Multipliers"),
            "#7C3AED"
        ) {
            setFieldText("partyHp", "9999999")
            setFieldText("partyMaxHp", "9999999")
            setFieldText("partyAtk", "999999")
            setFieldText("partyIsFixStatus", "1")
            listOf(
                "atkPerPhys", "atkPerMag", "defPerPhys", "defPerMag",
                "perAvoid", "decBadCond", "perAutoAtk", "perCrit",
                "perMulti", "perExpUp", "perHpUp", "perAutoHeal",
                "perFindTreasure", "perInjection", "addPerHit", "statusMaxPer"
            ).forEach { setFieldText(it, "500") }
            Toast.makeText(this, tr("已填入最强部队属性，点击【保存全部修改】即可写入存档", "最強部隊ステータスを入力しました", "Party preset filled!"), Toast.LENGTH_SHORT).show()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = dp(6)
            }
        }
        val presetBtn3 = makeStyledButton(
            tr("🌍 一键人类零伤亡 + 四国满兵力(999999) + 魔军削弱(100) + 兰斯舰队满战力", "🌍 人類被害ゼロ＋4大国兵力最大＋魔軍弱体化＋艦隊最大", "🌍 Zero Casualties + Max Human Armies + Weak Monster Armies"),
            "#059669"
        ) {
            setFieldText("humanCount", "300000")
            setFieldText("humanDead", "0")
            setFieldText("humanAlivePer", "100")
            setFieldText("humanDeadPer", "0")
            setFieldText("seaPlayerArmy", "999999")
            setFieldText("seaEnemyArmy", "100")
            setFieldText("leazasState", "0")
            setFieldText("helmanState", "0")
            setFieldText("zethState", "0")
            setFieldText("freeCitiesState", "0")
            setFieldText("leazasPArmy", "999999")
            setFieldText("helmanPArmy", "999999")
            setFieldText("zethPArmy", "999999")
            setFieldText("freeCitiesPArmy", "999999")
            setFieldText("leazasEArmy", "100")
            setFieldText("helmanEArmy", "100")
            setFieldText("zethEArmy", "100")
            setFieldText("freeCitiesEArmy", "100")
            setFieldText("subRemnant", "999999")
            setFieldText("subDwarf", "999999")
            setFieldText("subGeneral", "999999")
            setFieldText("subHanny", "999999")
            Toast.makeText(this, tr("已填入完美世界局势，点击【保存全部修改】即可写入存档", "世界情勢プリセットを入力しました", "World state preset filled!"), Toast.LENGTH_SHORT).show()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = dp(6)
            }
        }
        val presetBtn4 = makeStyledButton(
            tr("🃏 一键全卡牌+99叠卡 & +9999攻血 & 全角色5星满经验", "🃏 全カード+99枚＆攻HP+9999＆全キャラ星5・EXP最大", "🃏 All Cards x99 & +9999 Stats & Max All Characters"),
            "#DC2626"
        ) {
            editBatchSetCardCount.setText("99")
            editBatchBoostCardAtk.setText("9999")
            editBatchBoostCardHp.setText("9999")
            editBatchSetCharStar.setText("5")
            editBatchSetCharExp.setText("9999999")
            Toast.makeText(this, tr("已填入全卡牌与全角色强化参数，点击【保存全部修改】即可生效", "全カード・キャラ強化値を入力しました", "Card & Character batch values filled!"), Toast.LENGTH_SHORT).show()
        }
        presetCard.addView(presetBtn1)
        presetCard.addView(presetBtn2)
        presetCard.addView(presetBtn3)
        presetCard.addView(presetBtn4)
        container.addView(presetCard)

        // Category 1: Core Resources & Progress
        val resCard = makeCardContainer()
        resCard.addView(makeSectionTitle(tr("💎 1. 核心资源、回合与进度", "💎 1. 基本リソース・ターン・進行度", "💎 1. Core Resources & Turn Progress")))
        addLocalField(resCard, "turn", tr("当前回合数 (m_turn)", "現在ターン数 (m_turn)", "Current Turn (m_turn)"))
        addLocalField(resCard, "chapter", tr("当前章节 (m_chapter)", "チャプター (m_chapter)", "Chapter (m_chapter)"))
        addLocalField(resCard, "difficulty", tr("任务难度 (◆难度)", "クエスト難易度 (◆難易度)", "Quest Difficulty"))
        addLocalField(resCard, "battleCount", tr("战斗总回数 (◆战斗回数)", "総戦闘回数 (◆戦闘回数)", "Total Battle Count"))
        addLocalField(resCard, "foodTicket", tr("食券数量 (m_foodTicket)", "食券所持数 (m_foodTicket)", "Food Tickets (m_foodTicket)"))
        addLocalField(resCard, "honor", tr("勋章/功绩点 (m_honor)", "勲章数 (m_honor)", "Medals / Honor (m_honor)"))
        addLocalField(resCard, "friendPoint", tr("友情点数 (m_friendPoint)", "友好ポイント (m_friendPoint)", "Friendship Points (m_friendPoint)"))
        addLocalField(resCard, "totalExp", tr("累计总经验 (<TotalExp>)", "累計総経験値 (<TotalExp>)", "Total Party EXP (<TotalExp>)"))
        addLocalField(resCard, "partyRank", tr("部队等级 (<PartyRank>)", "部隊ランク (<PartyRank>)", "Party Rank (<PartyRank>)"))
        addLocalField(resCard, "itemStar", tr("装备品质星级 (<ItemStar>)", "装備アイテム星ランク (<ItemStar>)", "Equipment Star Rank (<ItemStar>)"))
        addLocalField(resCard, "treasureNotGetCount", tr("错失宝箱数 (<TreasureNotGetCount>, 0为全拿)", "取り逃し宝箱数 (<TreasureNotGetCount>)", "Missed Chest Count (<TreasureNotGetCount>)"))
        addLocalField(resCard, "partyBonusMaxPoint", tr("支援奖励最大点数 (m_maxPoint)", "部隊ボーナス最大Pt (m_maxPoint)", "Party Bonus Max Points (m_maxPoint)"))
        addLocalField(resCard, "isUsedHannyZippo", tr("哈尼打火机已用标记 (0=未用/可悔棋)", "ハニージッポー使用済フラグ (0=未使用)", "Hanny Zippo Used Flag (0=Unused)"))
        container.addView(resCard)

        // Category 2: Party HP & 16 CommonStatus Multipliers
        val partyCard = makeCardContainer()
        partyCard.addView(makeSectionTitle(tr("🛡 2. 部队生命与全 16 项战斗属性倍率", "🛡 2. 部隊HP＆全16種ステータス倍率", "🛡 2. Party HP & 16 Combat Multipliers")))
        addLocalField(partyCard, "partyHp", tr("当前部队 HP (g_party.m_hp)", "現在部隊HP (g_party.m_hp)", "Current Party HP (g_party.m_hp)"))
        addLocalField(partyCard, "partyMaxHp", tr("最大部队 HP (m_totalHp)", "最大部隊HP (m_totalHp)", "Max Party HP (m_totalHp)"))
        addLocalField(partyCard, "partyAtk", tr("部队面板总攻击 (<Atk>)", "部隊総攻撃力 (<Atk>)", "Total Party ATK (<Atk>)"))
        addLocalField(partyCard, "partyAvailCount", tr("出击人数上限 (m_availableCount)", "出撃可能枠数 (m_availableCount)", "Deploy Slot Count (m_availableCount)"))
        addLocalField(partyCard, "partyIsFixStatus", tr("锁定部队属性不被重算 (1=锁定, 0=默认)", "部隊ステータス固定 (1=再計算防止, 0=通常)", "Lock Party Status (<IsFixStatus>: 1=Lock)"))
        addLocalField(partyCard, "atkPerPhys", tr("物理攻击加成% (m_AtkPerPhysical)", "物理攻撃補正% (m_AtkPerPhysical)", "Physical ATK Bonus% (m_AtkPerPhysical)"))
        addLocalField(partyCard, "atkPerMag", tr("魔法攻击加成% (m_AtkPerMagical)", "魔法攻撃補正% (m_AtkPerMagical)", "Magical ATK Bonus% (m_AtkPerMagical)"))
        addLocalField(partyCard, "defPerPhys", tr("物理防御加成% (m_DefPerPhysical)", "物理防御補正% (m_DefPerPhysical)", "Physical DEF Bonus% (m_DefPerPhysical)"))
        addLocalField(partyCard, "defPerMag", tr("魔法防御加成% (m_DefPerMagical)", "魔法防御補正% (m_DefPerMagical)", "Magical DEF Bonus% (m_DefPerMagical)"))
        addLocalField(partyCard, "perCrit", tr("暴击率加成% (m_PerCritical)", "クリティカル率補正% (m_PerCritical)", "Critical Rate Bonus% (m_PerCritical)"))
        addLocalField(partyCard, "perMulti", tr("连击率加成% (m_PerMultiAttack)", "連撃率補正% (m_PerMultiAttack)", "Multi-Attack Bonus% (m_PerMultiAttack)"))
        addLocalField(partyCard, "addPerHit", tr("命中率加成% (m_AddPerHit)", "命中率補正% (m_AddPerHit)", "Hit Rate Bonus% (m_AddPerHit)"))
        addLocalField(partyCard, "perAvoid", tr("回避率加成% (m_PerAvoidance)", "回避率補正% (m_PerAvoidance)", "Avoidance Bonus% (m_PerAvoidance)"))
        addLocalField(partyCard, "perExpUp", tr("经验值加成% (m_PerExpUp)", "獲得経験値補正% (m_PerExpUp)", "EXP Gain Bonus% (m_PerExpUp)"))
        addLocalField(partyCard, "perHpUp", tr("HP上限加成% (m_PerHpUp)", "HP上限補正% (m_PerHpUp)", "Max HP Bonus% (m_PerHpUp)"))
        addLocalField(partyCard, "perAutoHeal", tr("回合自动回血% (m_PerAutoHeal)", "自動回復補正% (m_PerAutoHeal)", "Auto-Heal Bonus% (m_PerAutoHeal)"))
        addLocalField(partyCard, "decBadCond", tr("异常状态抗性% (m_DecPerBadCondition)", "状態異常耐性% (m_DecPerBadCondition)", "Status Resist Bonus% (m_DecPerBadCondition)"))
        addLocalField(partyCard, "perAutoAtk", tr("自动战斗伤害加成% (m_perAutoBattleAtk)", "オート戦闘攻撃補正% (m_perAutoBattleAtk)", "Auto-Battle ATK Bonus%"))
        addLocalField(partyCard, "perFindTreasure", tr("宝箱发现率加成% (m_PerFindTreasure)", "宝箱発見率補正% (m_PerFindTreasure)", "Treasure Find Bonus%"))
        addLocalField(partyCard, "perInjection", tr("远征支援伤害加成% (m_PerInjection)", "支援攻撃補正% (m_PerInjection)", "Support Injection Bonus%"))
        addLocalField(partyCard, "statusMaxPer", tr("属性上限突破% (StatusMaxPer)", "ステータス上限突破% (StatusMaxPer)", "Status Cap Bonus% (StatusMaxPer)"))
        container.addView(partyCard)

        // Category 3: World State, Population & Country Armies
        val worldCard = makeCardContainer()
        worldCard.addView(makeSectionTitle(tr("🌍 3. 人类人口、四国兵力与海战局势 (tt[])", "🌍 3. 人類人口・4大国兵力・海域戦力 (tt[])", "🌍 3. World Population, 4 Major Countries & Sea Forces")))
        addLocalField(worldCard, "humanCount", tr("人类存活人口 (tt[33])", "人類生存人口 (tt[33])", "Human Surviving Population (tt[33])"))
        addLocalField(worldCard, "humanDead", tr("人类死亡人口 (tt[34])", "人類死亡人口 (tt[34])", "Human Casualties (tt[34])"))
        addLocalField(worldCard, "humanAlivePer", tr("人类存活率% (tt[35])", "人類生存率% (tt[35])", "Human Survival Rate% (tt[35])"))
        addLocalField(worldCard, "humanDeadPer", tr("人类死亡率% (tt[36])", "人類死亡率% (tt[36])", "Human Death Rate% (tt[36])"))
        addLocalField(worldCard, "rareLuck", tr("稀有宝物幸运值 (tt[42])", "レア運 (tt[42])", "Rare Treasure Luck (tt[42])"))
        addLocalField(worldCard, "hannyZippo", tr("哈尼打火机剩余次数 (tt[101])", "ハニージッポー残数 (tt[101])", "Hanny Zippo Charges (tt[101])"))
        addLocalField(worldCard, "curseTurn", tr("魔人诅咒回合计数 (tt[41])", "呪いターン数 (tt[41])", "Curse Turn Count (tt[41])"))
        addLocalField(worldCard, "seaPlayerArmy", tr("海上兰斯舰队战力 (tt[66])", "海域ランス艦隊戦力 (tt[66])", "Sea Rance Fleet Power (tt[66])"))
        addLocalField(worldCard, "seaEnemyArmy", tr("海上魔物精锐战力 (tt[68])", "海域魔物精鋭戦力 (tt[68])", "Sea Monster Elite Power (tt[68])"))
        addLocalField(worldCard, "leazasState", tr("利萨斯状态 (tt[13]: 0=正常)", "リーザス状態 (tt[13]: 0=正常)", "Leazas State (tt[13]: 0=Normal)"))
        addLocalField(worldCard, "leazasPArmy", tr("利萨斯人类军兵力 (tt[25])", "リーザス人類軍兵力 (tt[25])", "Leazas Human Army (tt[25])"))
        addLocalField(worldCard, "leazasEArmy", tr("利萨斯魔军兵力 (tt[29])", "リーザス魔軍兵力 (tt[29])", "Leazas Monster Army (tt[29])"))
        addLocalField(worldCard, "helmanState", tr("赫尔曼状态 (tt[14]: 0=正常)", "ヘルマン状態 (tt[14]: 0=正常)", "Helman State (tt[14]: 0=Normal)"))
        addLocalField(worldCard, "helmanPArmy", tr("赫尔曼人类军兵力 (tt[26])", "ヘルマン人類軍兵力 (tt[26])", "Helman Human Army (tt[26])"))
        addLocalField(worldCard, "helmanEArmy", tr("赫尔曼魔军兵力 (tt[30])", "ヘルマン魔軍兵力 (tt[30])", "Helman Monster Army (tt[30])"))
        addLocalField(worldCard, "zethState", tr("赛斯状态 (tt[15]: 0=正常)", "ゼス状態 (tt[15]: 0=正常)", "Zeth State (tt[15]: 0=Normal)"))
        addLocalField(worldCard, "zethPArmy", tr("赛斯人类军兵力 (tt[27])", "ゼス人類軍兵力 (tt[27])", "Zeth Human Army (tt[27])"))
        addLocalField(worldCard, "zethEArmy", tr("赛斯魔军兵力 (tt[31])", "ゼス魔軍兵力 (tt[31])", "Zeth Monster Army (tt[31])"))
        addLocalField(worldCard, "freeCitiesState", tr("自由都市状态 (tt[16]: 0=正常)", "自由都市状態 (tt[16]: 0=正常)", "Free Cities State (tt[16]: 0=Normal)"))
        addLocalField(worldCard, "freeCitiesPArmy", tr("自由都市人类军兵力 (tt[28])", "自由都市人類軍兵力 (tt[28])", "Free Cities Human Army (tt[28])"))
        addLocalField(worldCard, "freeCitiesEArmy", tr("自由都市魔军兵力 (tt[32])", "自由都市魔軍兵力 (tt[32])", "Free Cities Monster Army (tt[32])"))
        addLocalField(worldCard, "subRemnant", tr("残余人类联军兵力 (tt[37])", "敗残兵人類軍兵力 (tt[37])", "Remnant Human Army (tt[37])"))
        addLocalField(worldCard, "subDwarf", tr("矮人军团兵力 (tt[38])", "ドワーフ軍兵力 (tt[38])", "Dwarf Army (tt[38])"))
        addLocalField(worldCard, "subGeneral", tr("名将护卫军兵力 (tt[39])", "名将軍兵力 (tt[39])", "General Army (tt[39])"))
        addLocalField(worldCard, "subHanny", tr("哈尼支援军兵力 (tt[40])", "ハニー軍兵力 (tt[40])", "Hanny Army (tt[40])"))
        container.addView(worldCard)

        // Category 4: Batch Card & Character Enhancement
        val batchCard = makeCardContainer()
        batchCard.addView(makeSectionTitle(tr("🃏 4. 全卡牌与全角色批量强化 (留空表示不修改该项)", "🃏 4. 全カード・全キャラ一括強化 (空欄は変更なし)", "🃏 4. Batch Card & Character Enhancement (Leave blank to skip)")))
        editBatchSetCardCount = addLabeledNumberField(
            batchCard,
            tr("将所有已获得卡牌的叠加张数统一设为 (如 99)", "全所持カードの枚数を一括設定 (例: 99)", "Set all owned card stack counts to (e.g. 99)"),
            ""
        )
        editBatchAddCardCount = addLabeledNumberField(
            batchCard,
            tr("为所有已获得卡牌额外增加叠加张数 (如 +50)", "全所持カードの枚数を一括加算 (例: 50)", "Add stack count to all owned cards (e.g. 50)"),
            ""
        )
        editBatchBoostCardAtk = addLabeledNumberField(
            batchCard,
            tr("为所有卡牌额外增加面板攻击力 (如 +5000)", "全カードの総攻撃力を一括加算 (例: 5000)", "Boost ATK of all cards by (e.g. 5000)"),
            ""
        )
        editBatchBoostCardHp = addLabeledNumberField(
            batchCard,
            tr("为所有卡牌额外增加面板生命值 (如 +5000)", "全カードの総HPを一括加算 (例: 5000)", "Boost HP of all cards by (e.g. 5000)"),
            ""
        )
        editBatchSetCharStar = addLabeledNumberField(
            batchCard,
            tr("将所有角色组突破星级统一设为 (如 5)", "全キャラの星ランクを一括設定 (例: 5)", "Set all character stars to (e.g. 5)"),
            ""
        )
        editBatchSetCharExp = addLabeledNumberField(
            batchCard,
            tr("将所有角色组经验值统一设为 (如 9999999)", "全キャラの経験値を一括設定 (例: 9999999)", "Set all character EXP to (e.g. 9999999)"),
            ""
        )
        val btnSaveBottom = makeStyledButton(
            tr("💾 保存全部修改到当前存档", "💾 変更を現在のセーブに保存", "💾 Save All Edits to Slot"),
            "#10B981"
        ) {
            saveCurrentLocalSlotModifications()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(12)
            }
        }
        batchCard.addView(btnSaveBottom)
        container.addView(batchCard)

        return container
    }

    private fun buildTab3FloatingAndLocks(): View {
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }

        val floatCard = makeCardContainer()
        floatCard.addView(makeSectionTitle(
            tr("🎈 游戏内悬浮窗实时修改器", "🎈 ゲーム内フローティング改造メニュー", "🎈 In-Game Floating Live Modifier")
        ))
        floatCard.addView(makeBodyText(
            tr(
                "开启后，运行游戏时屏幕边缘会出现可自由拖动的半透明「⚙ 修改」悬浮球。点击即可在游戏内随时开启：秒杀敌方、无限AP、锁满血、全员蓄力必暴、无限悔棋、实时改资源/兵力/全卡+99 等全部功能（无需系统级悬浮窗权限）。",
                "有効にすると、ゲーム画面上にドラッグ可能な半透明の「⚙ 改造」ボタンが表示されます。戦闘中の敵一撃撃破・無限AP・HP固定・全キャラブースト・リソース変更などをリアルタイムで行えます（特別な権限は不要です）。",
                "When enabled, a draggable translucent '⚙ MOD' button appears inside the game screen. Tap it anytime during gameplay or battle for 1-Hit Kill, Infinite AP, Full HP Lock, All Leaders Boost+Crit, and live resource/army editing."
            )
        ))

        switchFloatingWindow = Switch(this).apply {
            text = tr("在游戏内显示「⚙ 修改」悬浮窗按钮", "ゲーム内に「⚙ 改造」フローティングボタンを表示", "Show '⚙ MOD' Floating Button In-Game")
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            setPadding(0, dp(10), 0, dp(6))
            isChecked = GameModifierBridge.isFloatingEnabled(this@GameModifierActivity)
            setOnCheckedChangeListener { _, checked ->
                GameModifierBridge.setFloatingEnabled(this@GameModifierActivity, checked)
            }
        }
        floatCard.addView(switchFloatingWindow)
        container.addView(floatCard)

        val locksCard = makeCardContainer()
        locksCard.addView(makeSectionTitle(
            tr("🔒 游戏内 10 项实时锁定预设 (进入游戏自动生效，也可在悬浮窗随时开关)", "🔒 ゲーム内リアルタイム常時ロック設定 (10項目)", "🔒 10 Real-Time Per-Frame Locks (Auto-Applied In-Game)")
        ))

        val lockDefs = listOf(
            GameModifierBridge.LOCK_PARTY_MAX_HP to tr(
                "锁定我方部队满血 (无敌不死，HP 恒等于最大上限)",
                "味方部隊HP常時最大ロック（無敵）",
                "Lock Party Full HP (Invincible)"
            ),
            GameModifierBridge.LOCK_INFINITE_AP to tr(
                "锁定战斗无限 AP (行动点恒为 99，每回合无限出招)",
                "戦闘AP無限ロック（常時AP=99）",
                "Lock Infinite Battle AP (AP = 99)"
            ),
            GameModifierBridge.LOCK_BATTLE_COMBO to tr(
                "锁定战斗 999 连击倍率 (起手即享最高连击伤害加成)",
                "戦闘コンボ 999 固定ロック",
                "Lock Battle Combo = 999"
            ),
            GameModifierBridge.LOCK_BATTLE_ROUND_1 to tr(
                "锁定战斗当前回合为 1 & 回合上限 99 (永不因回合耗尽判负)",
                "戦闘ラウンド 1 固定＆上限 99（ターン切れ防止）",
                "Lock Battle Round = 1 & Max Round = 99"
            ),
            GameModifierBridge.LOCK_ONE_HIT_KILL to tr(
                "锁定敌方 1 血秒杀 (敌方登场或受击前 HP 自动变为 1，一击必杀)",
                "敵HP 1 固定ロック（一撃必殺）",
                "Lock Enemy HP = 1 (One-Hit Kill)"
            ),
            GameModifierBridge.LOCK_ENEMY_ZERO_ATK to tr(
                "锁定敌方 0 攻击 & 0 防御抗性 (敌方打我不掉血，护甲抗性归零)",
                "敵攻撃力0＆防御耐性0ロック",
                "Lock Enemy 0 ATK & 0 Defense/Resist"
            ),
            GameModifierBridge.LOCK_LEADER_BOOST_CRIT to tr(
                "锁定我方全员蓄力 Boost + 100% 暴击 + 100% 连击 + 自动解控",
                "味方全員常時ブースト＋100%クリティカル＋100%連撃＋状態異常無効",
                "Lock All Leaders Boosted + 100% Crit + 100% Multi-Attack + Cure Stun"
            ),
            GameModifierBridge.LOCK_INFINITE_SKILL_USE to tr(
                "锁定技能无限使用次数 (自动重置单场战斗技能次数限制)",
                "スキル使用回数無限ロック（回数制限スキルを毎フレームリセット）",
                "Lock Infinite Skill Uses Per Battle"
            ),
            GameModifierBridge.LOCK_INFINITE_ZIPPO to tr(
                "锁定无限哈尼打火机 (9次悔棋) & 自动清除诅咒回合",
                "ハニージッポー無限（常時9個）＆呪いターン0",
                "Lock Infinite Hanny Zippo (9) & 0 Curse Turn"
            ),
            GameModifierBridge.LOCK_MAX_RESOURCES to tr(
                "锁定食券 / 勋章 / 友情点数不低于 999",
                "食券・勲章・友好ポイントを 999 以上に維持",
                "Lock Food Tickets / Honor / Friend Points >= 999"
            )
        )

        val savedMask = GameModifierBridge.getSavedLockMask(this)
        for ((bit, label) in lockDefs) {
            val cb = CheckBox(this).apply {
                text = label
                setTextColor(Color.WHITE)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f)
                setPadding(dp(4), dp(6), dp(4), dp(6))
                isChecked = (savedMask and bit) != 0
                setOnCheckedChangeListener { _, _ ->
                    saveLockMaskFromUI()
                }
            }
            lockCheckBoxes[bit] = cb
            locksCard.addView(cb)
        }

        val quickRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(10)
            }
        }
        val btnEnableAllLocks = makeStyledButton(
            tr("⚡ 全选开启全部锁定", "⚡ すべてON", "⚡ Enable All Locks"),
            "#D97706"
        ) {
            lockCheckBoxes.values.forEach { it.isChecked = true }
            saveLockMaskFromUI()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                rightMargin = dp(6)
            }
        }
        val btnClearAllLocks = makeStyledButton(
            tr("✕ 全部关闭", "✕ すべてOFF", "✕ Disable All"),
            "#374151"
        ) {
            lockCheckBoxes.values.forEach { it.isChecked = false }
            saveLockMaskFromUI()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        quickRow.addView(btnEnableAllLocks)
        quickRow.addView(btnClearAllLocks)
        locksCard.addView(quickRow)

        container.addView(locksCard)
        return container
    }

    private fun saveLockMaskFromUI() {
        var mask = 0
        for ((bit, cb) in lockCheckBoxes) {
            if (cb.isChecked) mask = mask or bit
        }
        GameModifierBridge.setSavedLockMask(this, mask)
    }

    private fun refreshAllForCurrentTarget() {
        targetHeaderTitle.text = getString(R.string.save_manager_current_dir, currentTarget.displayName)
        targetHeaderSubtitle.text = "saves/${currentTarget.dirName}"

        refreshGlobalSaveStatus()
        reloadLocalSlots(keepCurrent = false)
    }

    private fun refreshGlobalSaveStatus() {
        val hasBak = GameModifierBridge.hasFullUnlockBackup(currentTarget.saveDir)
        btnRestoreUnlockBackup.visibility = if (hasBak) View.VISIBLE else View.GONE

        try {
            val raw = GameModifierBridge.readGlobalSaveSummaryForDir(currentTarget.saveDir)
            val json = JSONObject(raw)
            val hasGlobal = json.optBoolean("hasGlobal", false)
            val hasCol = json.optBoolean("hasCollection", false)
            val playCount = json.optInt("gamePlayCount", 0)
            val itemStar = json.optInt("itemStarRank", 0)
            val charCount = json.optInt("charRankCount", 0)
            val takeOverCount = json.optInt("takeOverCardCount", 0)
            val leaderAtkRatio = json.optInt("leaderAtkRatio", 5)
            val expUp = json.optInt("trophyExpBonus", 0)
            val statusMax = json.optInt("trophyStatusMaxPer", 0)

            globalStatusText.text = tr(
                "当前状态：GlobalSave=${if (hasGlobal) "已存在" else "未生成"} · Collection=${if (hasCol) "已存在" else "未生成"}\n" +
                    "周目数: $playCount · 全局星级: $itemStar · 联合部队角色记录: $charCount · 继承卡牌记录: $takeOverCount\n" +
                    "实绩攻击倍率: ${leaderAtkRatio}% · 实绩经验加成: +${expUp}% · 实绩上限突破: +${statusMax}%",
                "現在の状態：GlobalSave=${if (hasGlobal) "あり" else "なし"} · Collection=${if (hasCol) "あり" else "なし"}\n" +
                    "周回数: $playCount · 最高星ランク: $itemStar · 連合部隊キャラ数: $charCount · 引継ぎカード数: $takeOverCount",
                "Status: GlobalSave=${if (hasGlobal) "Found" else "Missing"} · Collection=${if (hasCol) "Found" else "Missing"}\n" +
                    "Playthroughs: $playCount · Item Star: $itemStar · AF Chars: $charCount · TakeOver Cards: $takeOverCount"
            )
            if (hasGlobal) {
                editGlobalPlayCount.setText(playCount.toString())
                editGlobalItemStar.setText(itemStar.toString())
            }
            if (hasCol) {
                editGlobalLeaderAtkRatio.setText(leaderAtkRatio.toString())
                editGlobalExpUp.setText(expUp.toString())
                editGlobalStatusMax.setText(statusMax.toString())
            }
        } catch (e: Throwable) {
            globalStatusText.text = tr("尚未读取到全局存档（可先一键导入100%全解锁存档）", "全体セーブが見つかりません", "No GlobalSave found yet.")
        }
    }

    private fun applyGlobalSaveModification() {
        val spec = JSONObject()
        editGlobalPlayCount.text.toString().trim().toIntOrNull()?.let { spec.put("gamePlayCount", it) }
        editGlobalItemStar.text.toString().trim().toIntOrNull()?.let { spec.put("itemStarRank", it) }
        editGlobalLeaderAtkRatio.text.toString().trim().toIntOrNull()?.let { spec.put("leaderAtkRatio", it) }
        editGlobalExpUp.text.toString().trim().toIntOrNull()?.let { spec.put("trophyExpBonus", it) }
        editGlobalStatusMax.text.toString().trim().toIntOrNull()?.let { spec.put("trophyStatusMaxPer", it) }
        if (chkGlobalMaxCharRank.isChecked) spec.put("setAllCharRank", 10)
        if (chkGlobalMaxTakeOver.isChecked) spec.put("setTakeOverCardCount", 99)

        val ok = GameModifierBridge.writeGlobalSaveModForDir(currentTarget.saveDir, spec.toString())
        if (ok) {
            refreshGlobalSaveStatus()
            Toast.makeText(
                this,
                tr("已成功修改并保存 GlobalSave / Collection！", "全体・コレクションセーブを保存しました！", "GlobalSave / Collection updated!"),
                Toast.LENGTH_LONG
            ).show()
        } else {
            Toast.makeText(
                this,
                tr("未找到 GlobalSave.asd 或 Collection.asd，请先点击上方【一键安装 100% 全解锁存档】", "GlobalSave.asd が見つかりません。先に100%全解放セーブを適用してください", "GlobalSave.asd not found. Install 100% Full Unlock Save first."),
                Toast.LENGTH_LONG
            ).show()
        }
    }

    private fun reloadLocalSlots(keepCurrent: Boolean) {
        val prevFile = selectedSlot?.file
        val foundFiles = mutableListOf<File>()
        currentTarget.saveDir.walkTopDown().maxDepth(2).forEach { f ->
            if (f.isFile && f.name.startsWith("LocalSave", ignoreCase = true) && f.name.endsWith(".asd", ignoreCase = true)) {
                foundFiles.add(f)
            }
        }
        val regex = Regex("LocalSave(\\d+)\\.asd", RegexOption.IGNORE_CASE)
        val slots = foundFiles.map { file ->
            val num = regex.find(file.name)?.groupValues?.getOrNull(1)?.toIntOrNull() ?: 999999
            val summary = try {
                val raw = GameModifierBridge.nativeReadLocalSaveSummary(file.absolutePath)
                JSONObject(raw).takeIf { it.optBoolean("valid", false) }
            } catch (_: Throwable) {
                null
            }
            LocalSaveSlot(file, num, summary)
        }.sortedWith(compareBy<LocalSaveSlot> { it.slotNumber }.thenBy { it.file.name })

        localSlots = slots
        val chosen = if (keepCurrent && prevFile != null) {
            slots.firstOrNull { it.file.absolutePath == prevFile.absolutePath } ?: slots.firstOrNull()
        } else {
            slots.firstOrNull()
        }
        populateSlotToForm(chosen)
    }

    private fun formatSlotLabel(slot: LocalSaveSlot): String {
        val s = slot.summary
        val comment = s?.optString("comment", "")?.replace('\n', ' ')?.trim().orEmpty()
        val turn = s?.optInt("turn", 0) ?: 0
        val typeTag = if (slot.slotNumber >= 5000) {
            tr("自动存档", "オート", "Auto")
        } else {
            tr("普通存档", "通常", "Slot")
        }
        return if (comment.isNotEmpty()) {
            "${slot.file.name} [$typeTag · Turn $turn] $comment"
        } else {
            "${slot.file.name} [$typeTag · Turn $turn]"
        }
    }

    private fun populateSlotToForm(slot: LocalSaveSlot?) {
        selectedSlot = slot
        if (slot == null) {
            slotTitleText.text = tr("当前存档目录下暂无 LocalSave*.asd 进度存档", "LocalSave*.asd が見つかりません", "No LocalSave*.asd found in current directory")
            slotCommentText.text = tr(
                "您可以先启动游戏保存一次进度，或在存档管理器中导入进度存档；也可直接使用【全解锁】与【游戏内悬浮窗实时修改】。",
                "ゲーム内でセーブを作成するか、セーブ管理からインポートしてください。ゲーム内オーバーレイ改造はすぐに使用可能です。",
                "Save once in-game or import a save in Save Manager, or use the In-Game Floating Modifier directly."
            )
            btnRestoreSlotBackup.visibility = View.GONE
            localFields.values.forEach { it.setText("") }
            return
        }

        val df = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault())
        slotTitleText.text = formatSlotLabel(slot)
        val s = slot.summary
        val ownedCards = s?.optInt("cardCount", 0) ?: 0
        val charGroups = s?.optInt("charCount", 0) ?: 0
        slotCommentText.text = tr(
            "文件: ${slot.file.name} · 修改时间: ${df.format(Date(slot.file.lastModified()))}\n已持有卡牌: $ownedCards 张 · 角色组记录: $charGroups 组",
            "ファイル: ${slot.file.name} · 更新: ${df.format(Date(slot.file.lastModified()))}\n所持カード: $ownedCards 枚 · キャラ枠: $charGroups",
            "File: ${slot.file.name} · Modified: ${df.format(Date(slot.file.lastModified()))}\nOwned Cards: $ownedCards · Character Groups: $charGroups"
        )
        btnRestoreSlotBackup.visibility = if (GameModifierBridge.hasModBackup(slot.file)) View.VISIBLE else View.GONE

        if (s != null) {
            for ((key, edit) in localFields) {
                if (s.has(key)) {
                    edit.setText(s.optInt(key, 0).toString())
                } else {
                    edit.setText("")
                }
            }
        }
        editBatchSetCardCount.setText("")
        editBatchAddCardCount.setText("")
        editBatchBoostCardAtk.setText("")
        editBatchBoostCardHp.setText("")
        editBatchSetCharStar.setText("")
        editBatchSetCharExp.setText("")
    }

    private fun showSelectLocalSlotDialog() {
        if (localSlots.isEmpty()) {
            Toast.makeText(this, tr("当前目录没有 LocalSave*.asd 进度存档", "LocalSave*.asd がありません", "No LocalSave*.asd slots found"), Toast.LENGTH_SHORT).show()
            return
        }
        val labels = localSlots.map { formatSlotLabel(it) }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle(tr("选择要修改的进度存档", "編集するセーブスロットを選択", "Select Save Slot to Edit"))
            .setItems(labels) { _, which ->
                populateSlotToForm(localSlots[which])
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun saveCurrentLocalSlotModifications() {
        val slot = selectedSlot
        if (slot == null) {
            Toast.makeText(this, tr("请先选择一个 LocalSave 进度存档", "先にセーブスロットを選択してください", "Please select a LocalSave slot first"), Toast.LENGTH_SHORT).show()
            return
        }

        val spec = JSONObject()
        for ((key, edit) in localFields) {
            val v = edit.text.toString().trim().toIntOrNull()
            if (v != null) {
                spec.put(key, v)
            }
        }
        editBatchSetCardCount.text.toString().trim().toIntOrNull()?.let { spec.put("setCardCount", it) }
        editBatchAddCardCount.text.toString().trim().toIntOrNull()?.let { spec.put("addCardCount", it) }
        editBatchBoostCardAtk.text.toString().trim().toIntOrNull()?.let { spec.put("cardAtkBonus", it) }
        editBatchBoostCardHp.text.toString().trim().toIntOrNull()?.let { spec.put("cardHpBonus", it) }
        editBatchSetCharStar.text.toString().trim().toIntOrNull()?.let { spec.put("setCharStar", it) }
        editBatchSetCharExp.text.toString().trim().toIntOrNull()?.let { spec.put("setCharExp", it) }

        GameModifierBridge.backupSaveFileBeforeMod(slot.file)
        val ok = GameModifierBridge.nativeWriteLocalSaveMod(slot.file.absolutePath, spec.toString())
        if (ok) {
            reloadLocalSlots(keepCurrent = true)
            Toast.makeText(
                this,
                tr("已成功保存修改到 ${slot.file.name}（原存档已自动备份）！", "${slot.file.name} に変更を保存しました！", "Saved modifications to ${slot.file.name}!"),
                Toast.LENGTH_LONG
            ).show()
        } else {
            Toast.makeText(this, tr("保存修改失败，请检查存档文件是否有效", "保存に失敗しました", "Failed to write save modifications"), Toast.LENGTH_LONG).show()
        }
    }

    private fun showSwitchTargetDialog() {
        discoverSaveTargets(currentTarget.dirName)
        val labels = targets.map { it.displayName }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle(R.string.save_dir_select_title)
            .setItems(labels) { _, which ->
                currentTarget = targets[which]
                refreshAllForCurrentTarget()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun setFieldText(key: String, value: String) {
        localFields[key]?.setText(value)
    }

    private fun addLocalField(parent: LinearLayout, key: String, label: String) {
        val edit = addLabeledNumberField(parent, label, "")
        localFields[key] = edit
    }

    private fun addLabeledNumberField(parent: LinearLayout, label: String, defaultVal: String): EditText {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(3), 0, dp(3))
        }
        val tv = TextView(this).apply {
            text = label
            setTextColor(Color.parseColor("#E5E7EB"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.35f)
        }
        val edit = EditText(this).apply {
            setText(defaultVal)
            setTextColor(Color.WHITE)
            setHintTextColor(Color.parseColor("#6B7280"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f)
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED
            setPadding(dp(10), dp(6), dp(10), dp(6))
            background = GradientDrawable().apply {
                cornerRadius = dp(6).toFloat()
                setColor(Color.parseColor("#111827"))
                setStroke(dp(1), Color.parseColor("#374151"))
            }
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.85f)
        }
        row.addView(tv)
        row.addView(edit)
        parent.addView(row)
        return edit
    }

    private fun makeCardContainer(): LinearLayout {
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(14), dp(12), dp(14), dp(14))
            background = GradientDrawable().apply {
                cornerRadius = dp(10).toFloat()
                setColor(Color.parseColor("#1E2430"))
                setStroke(dp(1), Color.parseColor("#2E3748"))
            }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                bottomMargin = dp(12)
            }
        }
    }

    private fun makeSectionTitle(textStr: String): TextView {
        return TextView(this).apply {
            text = textStr
            setTextColor(Color.parseColor("#F59E0B"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            setTypeface(typeface, Typeface.BOLD)
            setPadding(0, 0, 0, dp(6))
        }
    }

    private fun makeBodyText(textStr: String): TextView {
        return TextView(this).apply {
            text = textStr
            setTextColor(Color.parseColor("#D1D5DB"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f)
            setLineSpacing(dp(2).toFloat(), 1.05f)
        }
    }

    private fun makeStyledButton(textStr: String, hexColor: String, onClick: () -> Unit): Button {
        return Button(this).apply {
            text = textStr
            isAllCaps = false
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f)
            setTypeface(typeface, Typeface.BOLD)
            setPadding(dp(12), dp(8), dp(12), dp(8))
            background = GradientDrawable().apply {
                cornerRadius = dp(8).toFloat()
                setColor(Color.parseColor(hexColor))
            }
            setOnClickListener { onClick() }
        }
    }
}
