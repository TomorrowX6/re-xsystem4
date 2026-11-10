package io.github.kichikuou.xsystem4

import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.util.Locale
import kotlin.math.abs

class FloatingModifierOverlay(private val activity: Activity) {

    private val handler = Handler(Looper.getMainLooper())
    private var rootOverlay: FrameLayout? = null
    private var floatingBtn: TextView? = null
    private var panelContainer: LinearLayout? = null
    private var statusBanner: TextView? = null

    private val tabButtons = mutableListOf<Button>()
    private val tabPages = mutableListOf<View>()
    private var currentTab = 0

    private val lockCheckBoxes = LinkedHashMap<Int, CheckBox>()
    private val liveFields = LinkedHashMap<String, EditText>()

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
        (v * activity.resources.displayMetrics.density).toInt()

    private val periodicRefreshRunnable = object : Runnable {
        override fun run() {
            if (panelContainer?.visibility == View.VISIBLE) {
                refreshStatusBannerOnly()
                handler.postDelayed(this, 800L)
            }
        }
    }

    fun attach() {
        GameModifierBridge.ensureLoaded()
        val initialLocks = GameModifierBridge.getSavedLockMask(activity)
        try {
            GameModifierBridge.nativeSetLiveLocks(initialLocks)
        } catch (_: Throwable) {
        }

        if (!GameModifierBridge.isFloatingEnabled(activity)) {
            return
        }

        val container = FrameLayout(activity).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
            isClickable = false
            isFocusable = false
        }
        rootOverlay = container

        val btn = buildFloatingButton()
        floatingBtn = btn

        val panel = buildModifierPanel()
        panel.visibility = View.GONE
        panelContainer = panel

        container.addView(panel)
        container.addView(btn)

        activity.addContentView(
            container,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        )
    }

    fun detach() {
        handler.removeCallbacksAndMessages(null)
    }

    private fun buildFloatingButton(): TextView {
        val btn = TextView(activity).apply {
            text = tr("⚙ 修改", "⚙ 改造", "⚙ MOD")
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f)
            setTypeface(typeface, Typeface.BOLD)
            gravity = Gravity.CENTER
            setPadding(dp(12), dp(7), dp(12), dp(7))
            background = GradientDrawable().apply {
                cornerRadius = dp(20).toFloat()
                setColor(Color.parseColor("#CC1E293B"))
                setStroke(dp(1), Color.parseColor("#F59E0B"))
            }
            alpha = 0.85f
        }

        val lp = FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.START
        ).apply {
            leftMargin = dp(16)
            topMargin = dp(20)
        }
        btn.layoutParams = lp

        var downRawX = 0f
        var downRawY = 0f
        var startLeft = 0
        var startTop = 0
        var dragging = false

        btn.setOnTouchListener { v, event ->
            val params = v.layoutParams as FrameLayout.LayoutParams
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downRawX = event.rawX
                    downRawY = event.rawY
                    startLeft = params.leftMargin
                    startTop = params.topMargin
                    dragging = false
                    v.alpha = 1.0f
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = (event.rawX - downRawX).toInt()
                    val dy = (event.rawY - downRawY).toInt()
                    if (abs(dx) > dp(6) || abs(dy) > dp(6)) {
                        dragging = true
                    }
                    if (dragging) {
                        val parentW = (rootOverlay?.width ?: activity.resources.displayMetrics.widthPixels)
                        val parentH = (rootOverlay?.height ?: activity.resources.displayMetrics.heightPixels)
                        params.leftMargin = (startLeft + dx).coerceIn(0, (parentW - v.width).coerceAtLeast(0))
                        params.topMargin = (startTop + dy).coerceIn(0, (parentH - v.height).coerceAtLeast(0))
                        v.layoutParams = params
                    }
                    true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    v.alpha = 0.85f
                    if (!dragging) {
                        togglePanel()
                    }
                    true
                }
                else -> false
            }
        }
        return btn
    }

    private fun togglePanel() {
        val panel = panelContainer ?: return
        if (panel.visibility == View.VISIBLE) {
            closePanel()
        } else {
            openPanel()
        }
    }

    private fun openPanel() {
        val panel = panelContainer ?: return
        panel.visibility = View.VISIBLE
        syncLocksToCheckboxes()
        refreshAllLiveValuesIntoForm()
        handler.removeCallbacks(periodicRefreshRunnable)
        handler.postDelayed(periodicRefreshRunnable, 800L)
    }

    private fun closePanel() {
        handler.removeCallbacks(periodicRefreshRunnable)
        panelContainer?.visibility = View.GONE
    }

    private fun buildModifierPanel(): LinearLayout {
        val dm = activity.resources.displayMetrics
        val panelW = (dm.widthPixels * 0.82f).toInt().coerceAtMost(dp(680))
        val panelH = (dm.heightPixels * 0.86f).toInt().coerceAtMost(dp(440))

        val panel = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            isClickable = true
            isFocusable = true
            background = GradientDrawable().apply {
                cornerRadius = dp(12).toFloat()
                setColor(Color.parseColor("#EB111827"))
                setStroke(dp(1), Color.parseColor("#D97706"))
            }
            layoutParams = FrameLayout.LayoutParams(panelW, panelH, Gravity.CENTER)
        }

        // Top Bar
        val topBar = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(12), dp(8), dp(12), dp(8))
            setBackgroundColor(Color.parseColor("#1F2937"))
        }
        val titleTv = TextView(activity).apply {
            text = tr("⚡ 兰斯10 实时修改器", "⚡ ランス10 リアルタイム改造", "⚡ Rance 10 Live Modifier")
            setTextColor(Color.parseColor("#FBBF24"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            setTypeface(typeface, Typeface.BOLD)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        val btnRefresh = makeSmallButton(tr("🔄 刷新读取", "🔄 更新", "🔄 Refresh"), "#2563EB") {
            refreshAllLiveValuesIntoForm()
            Toast.makeText(activity, tr("已同步游戏内最新数值", "最新値を取得しました", "Refreshed live state"), Toast.LENGTH_SHORT).show()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                rightMargin = dp(6)
            }
        }
        val btnHideFloat = makeSmallButton(tr("隐藏悬浮球", "ボタン非表示", "Hide Btn"), "#4B5563") {
            closePanel()
            floatingBtn?.visibility = View.GONE
            Toast.makeText(
                activity,
                tr("本次运行已隐藏悬浮球（实时锁定继续生效）", "今回の起動中はボタンを非表示にしました", "Floating button hidden for this session"),
                Toast.LENGTH_SHORT
            ).show()
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                rightMargin = dp(6)
            }
        }
        val btnClose = makeSmallButton(tr("✕ 关闭", "✕ 閉じる", "✕ Close"), "#DC2626") {
            closePanel()
        }
        topBar.addView(titleTv)
        topBar.addView(btnRefresh)
        topBar.addView(btnHideFloat)
        topBar.addView(btnClose)
        panel.addView(topBar)

        // Live Status Banner
        statusBanner = TextView(activity).apply {
            setTextColor(Color.parseColor("#A7F3D0"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f)
            setBackgroundColor(Color.parseColor("#141E2E"))
            setPadding(dp(12), dp(4), dp(12), dp(4))
        }
        panel.addView(statusBanner)

        // Sub-Tab Row
        val tabRow = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Color.parseColor("#18202F"))
            setPadding(dp(6), dp(4), dp(6), dp(4))
        }
        val tabLabels = listOf(
            tr("⚔ 战斗与锁定", "⚔ 戦闘・ロック", "⚔ Battle & Locks"),
            tr("💎 资源与进度", "💎 リソース・進行", "💎 Resources"),
            tr("🃏 部队与卡牌", "🃏 部隊・カード", "🃏 Party & Cards"),
            tr("🌍 四国与人口", "🌍 世界・兵力", "🌍 World & Armies")
        )
        tabLabels.forEachIndexed { idx, label ->
            val b = Button(activity).apply {
                text = label
                isAllCaps = false
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
                setPadding(dp(4), dp(5), dp(4), dp(5))
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                    setMargins(dp(3), 0, dp(3), 0)
                }
                setOnClickListener { selectPanelTab(idx) }
            }
            tabButtons.add(b)
            tabRow.addView(b)
        }
        panel.addView(tabRow)

        val scroll = ScrollView(activity).apply {
            isFillViewport = true
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
        }
        val pagesWrap = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(10), dp(8), dp(10), dp(14))
        }

        val p1 = buildPanelTab1BattleAndLocks()
        val p2 = buildPanelTab2Resources()
        val p3 = buildPanelTab3PartyAndCards()
        val p4 = buildPanelTab4World()
        tabPages.addAll(listOf(p1, p2, p3, p4))
        pagesWrap.addView(p1)
        pagesWrap.addView(p2)
        pagesWrap.addView(p3)
        pagesWrap.addView(p4)
        scroll.addView(pagesWrap)
        panel.addView(scroll)

        selectPanelTab(0)
        return panel
    }

    private fun selectPanelTab(index: Int) {
        currentTab = index
        tabPages.forEachIndexed { idx, view ->
            view.visibility = if (idx == index) View.VISIBLE else View.GONE
        }
        tabButtons.forEachIndexed { idx, btn ->
            btn.background = GradientDrawable().apply {
                cornerRadius = dp(6).toFloat()
                setColor(if (idx == index) Color.parseColor("#D97706") else Color.parseColor("#283245"))
            }
            btn.setTextColor(if (idx == index) Color.WHITE else Color.parseColor("#9CA3AF"))
            btn.setTypeface(null, if (idx == index) Typeface.BOLD else Typeface.NORMAL)
        }
    }

    private fun buildPanelTab1BattleAndLocks(): View {
        val col = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }

        col.addView(makeSubHeader(tr("⚡ 战斗瞬发指令 (点击立即在当前帧生效)", "⚡ 戦闘即時アクション", "⚡ Instant Battle Actions")))

        val row1 = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        row1.addView(makeActionGridButton(tr("💀 秒杀敌方 (敌HP=1)", "💀 敵HP=1 (一撃)", "💀 Enemy HP=1"), "#DC2626") {
            sendLiveAction(JSONObject().put("killEnemy", true), tr("敌方HP已设为 1！随意攻击即可秒杀", "敵HPを1にしました！", "Enemy HP set to 1!"))
        })
        row1.addView(makeActionGridButton(tr("🛡 废除敌攻 (敌攻=0)", "🛡 敵攻撃=0", "🛡 Enemy ATK=0"), "#7C3AED") {
            sendLiveAction(JSONObject().put("weakenEnemy", true), tr("敌方攻击力已归零！", "敵攻撃力を0にしました！", "Enemy ATK set to 0!"))
        })
        row1.addView(makeActionGridButton(tr("💚 我方立即满血", "💚 味方HP全快", "💚 Full Party HP"), "#059669") {
            sendLiveAction(JSONObject().put("refillPartyHp", true), tr("我方部队生命已回满！", "味方HPを全回復しました！", "Party HP refilled!"))
        })
        col.addView(row1)

        val row2 = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(5)
                bottomMargin = dp(8)
            }
        }
        row2.addView(makeActionGridButton(tr("🔥 AP 设为 99", "🔥 AP=99", "🔥 Set AP=99"), "#D97706") {
            sendLiveAction(JSONObject().put("battleAp", 99), tr("当前 AP 已设为 99！", "APを99にしました！", "Battle AP set to 99!"))
        })
        row2.addView(makeActionGridButton(tr("💥 连击设为 999", "💥 コンボ=999", "💥 Combo=999"), "#2563EB") {
            sendLiveAction(JSONObject().put("battleCombo", 999), tr("当前连击已设为 999！", "コンボを999にしました！", "Battle Combo set to 999!"))
        })
        row2.addView(makeActionGridButton(tr("✨ 全员蓄力必暴+解控", "✨ 全員ブースト+回復", "✨ Boost+Crit+Cure All"), "#0D9488") {
            sendLiveAction(
                JSONObject().put("cureAndBoostLeaders", true).put("battleRound", 1),
                tr("全员已开启蓄力Boost+必暴+必连+重置技能次数！", "味方全員ブースト＆スキル回数リセット完了！", "All leaders boosted & skill cooldowns reset!")
            )
        })
        col.addView(row2)

        col.addView(makeSubHeader(tr("🔒 10 项实时每帧锁定开关 (勾选即常驻生效)", "🔒 リアルタイム常時ロック (10項目)", "🔒 10 Continuous Per-Frame Locks")))

        val lockDefs = listOf(
            GameModifierBridge.LOCK_PARTY_MAX_HP to tr("锁定我方部队满血 (无敌不死)", "味方HP常時最大ロック（無敵）", "Lock Party Full HP (Invincible)"),
            GameModifierBridge.LOCK_INFINITE_AP to tr("锁定战斗无限 AP (恒为 99)", "戦闘AP無限ロック (AP=99)", "Lock Infinite Battle AP (99)"),
            GameModifierBridge.LOCK_BATTLE_COMBO to tr("锁定战斗 999 连击伤害加成", "戦闘コンボ 999 ロック", "Lock Battle Combo = 999"),
            GameModifierBridge.LOCK_BATTLE_ROUND_1 to tr("锁定战斗回合数为 1 & 上限 99", "戦闘ラウンド 1 固定", "Lock Battle Round = 1"),
            GameModifierBridge.LOCK_ONE_HIT_KILL to tr("锁定敌方 1 血秒杀 (一击必杀)", "敵HP 1 ロック（一撃必殺）", "Lock Enemy HP = 1 (1-Hit Kill)"),
            GameModifierBridge.LOCK_ENEMY_ZERO_ATK to tr("锁定敌方 0 攻击 & 0 防御抗性", "敵攻撃0＆防御耐性0ロック", "Lock Enemy 0 ATK & 0 Def/Resist"),
            GameModifierBridge.LOCK_LEADER_BOOST_CRIT to tr("锁定全员蓄力 Boost + 100%必暴必连 + 免疫异常", "味方全員常時ブースト＋必殺＋連撃＋異常無効", "Lock All Leaders Boost + 100% Crit/Multi + Immune"),
            GameModifierBridge.LOCK_INFINITE_SKILL_USE to tr("锁定单场战斗技能无限使用次数", "スキル使用回数無限ロック", "Lock Infinite Skill Uses"),
            GameModifierBridge.LOCK_INFINITE_ZIPPO to tr("锁定无限哈尼打火机(9次悔棋) & 0诅咒", "ハニージッポー無限(9個)＆呪い0", "Lock Infinite Hanny Zippo (9)"),
            GameModifierBridge.LOCK_MAX_RESOURCES to tr("锁定食券 / 勋章 / 友情点 ≥ 999", "食券・勲章・友好Pt 999以上維持", "Lock Food/Honor/Friend >= 999")
        )

        for ((bit, label) in lockDefs) {
            val cb = CheckBox(activity).apply {
                text = label
                setTextColor(Color.WHITE)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f)
                setPadding(dp(4), dp(3), dp(4), dp(3))
                setOnCheckedChangeListener { _, _ ->
                    updateLocksFromOverlayUI()
                }
            }
            lockCheckBoxes[bit] = cb
            col.addView(cb)
        }

        return col
    }

    private fun buildPanelTab2Resources(): View {
        val col = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }

        val btnMaxRes = makeSmallButton(
            tr("💰 一键实时满资源 (食券/勋章/友情=9999, 经验=99999999, 装备星=20, 9次悔棋)", "💰 ワンクリック全リソース最大適用", "💰 Instant Max Resources, EXP & Item Star"),
            "#D97706"
        ) {
            val act = JSONObject()
                .put("foodTicket", 9999)
                .put("honor", 9999)
                .put("friendPoint", 9999)
                .put("totalExp", 99999999)
                .put("partyRank", 999)
                .put("itemStar", 20)
                .put("treasureNotGetCount", 0)
                .put("partyBonusMaxPoint", 999)
                .put("isUsedHannyZippo", 0)
                .put("hannyZippo", 9)
                .put("curseTurn", 0)
                .put("rareLuck", 999)
            sendLiveAction(act, tr("已实时将全部资源、经验与装备星级拉满！", "全リソースを最大化しました！", "All resources maxed in live memory!"))
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = dp(8)
            }
        }
        col.addView(btnMaxRes)

        addLiveFieldRow(col, "turn", tr("当前回合数 (m_turn)", "ターン数 (m_turn)", "Turn (m_turn)"))
        addLiveFieldRow(col, "difficulty", tr("当前任务难度 (◆难度)", "クエスト難易度", "Quest Difficulty"))
        addLiveFieldRow(col, "battleCount", tr("战斗回数 (◆战斗回数)", "総戦闘回数", "Battle Count"))
        addLiveFieldRow(col, "foodTicket", tr("食券数量 (m_foodTicket)", "食券数", "Food Tickets"))
        addLiveFieldRow(col, "honor", tr("勋章数量 (m_honor)", "勲章数", "Medals / Honor"))
        addLiveFieldRow(col, "friendPoint", tr("友情点数 (m_friendPoint)", "友好ポイント", "Friend Points"))
        addLiveFieldRow(col, "totalExp", tr("累计总经验 (<TotalExp>)", "累計総経験値", "Total EXP"))
        addLiveFieldRow(col, "partyRank", tr("部队等级 (<PartyRank>)", "部隊ランク", "Party Rank"))
        addLiveFieldRow(col, "itemStar", tr("装备星级 (<ItemStar>)", "装備星ランク", "Item Star"))
        addLiveFieldRow(col, "treasureNotGetCount", tr("错失宝箱数 (0=完美)", "取り逃し宝箱数", "Missed Chests"))
        addLiveFieldRow(col, "partyBonusMaxPoint", tr("支援奖励点数 (m_maxPoint)", "部隊ボーナス最大Pt", "Party Bonus Max Pt"))
        addLiveFieldRow(col, "hannyZippo", tr("哈尼打火机次数 (tt[101])", "ハニージッポー残数", "Hanny Zippo Count"))
        addLiveFieldRow(col, "curseTurn", tr("诅咒回合数 (tt[41])", "呪いターン数", "Curse Turn"))
        addLiveFieldRow(col, "rareLuck", tr("稀有宝物运 (tt[42])", "レア運", "Rare Luck"))

        val btnApplyRes = makeSmallButton(
            tr("💾 立即写入以上资源与进度修改", "💾 上記のリソース変更を即時反映", "💾 Apply Resource Edits Now"),
            "#10B981"
        ) {
            applySelectedLiveFields(
                listOf(
                    "turn", "difficulty", "battleCount", "foodTicket", "honor", "friendPoint",
                    "totalExp", "partyRank", "itemStar", "treasureNotGetCount",
                    "partyBonusMaxPoint", "hannyZippo", "curseTurn", "rareLuck"
                )
            )
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(8)
            }
        }
        col.addView(btnApplyRes)
        return col
    }

    private fun buildPanelTab3PartyAndCards(): View {
        val col = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }

        val btnSuperParty = makeSmallButton(
            tr("🛡 一键最强部队 (HP=9999999 & 全16项攻防暴连倍率+500%)", "🛡 ワンクリック最強部隊ステータス反映", "🛡 Instant 9999999 HP & +500% All 16 Multipliers"),
            "#7C3AED"
        ) {
            val act = JSONObject()
                .put("partyHp", 9999999)
                .put("partyMaxHp", 9999999)
                .put("partyAtk", 999999)
                .put("partyIsFixStatus", 1)
                .put("allStatusPer", 500)
            sendLiveAction(act, tr("已实时生效最强部队生命与全16项500%属性加成！", "最強部隊ステータスを即時反映しました！", "Max Party HP & +500% multipliers applied!"))
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = dp(6)
            }
        }
        col.addView(btnSuperParty)

        val btnSuperCards = makeSmallButton(
            tr("🃏 一键全持有卡牌叠满 99 张 & +9999 攻血 & 全角色5星满经验", "🃏 全所持カード99枚＆攻HP+9999＆全キャラ星5・EXP最大", "🃏 Instant All Owned Cards x99, +9999 Stats & Max Chars"),
            "#DC2626"
        ) {
            val act = JSONObject()
                .put("setCardCount", 99)
                .put("cardAtkBonus", 9999)
                .put("cardHpBonus", 9999)
                .put("setCharStar", 5)
                .put("setCharExp", 9999999)
            sendLiveAction(act, tr("全持有卡牌已升至99张并附加+9999攻血，全角色已满星满经验！", "全カード＆全キャラを一括強化しました！", "All cards & characters maxed in live memory!"))
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = dp(8)
            }
        }
        col.addView(btnSuperCards)

        addLiveFieldRow(col, "partyHp", tr("当前部队 HP", "現在部隊HP", "Current Party HP"))
        addLiveFieldRow(col, "partyMaxHp", tr("最大部队 HP", "最大部隊HP", "Max Party HP"))
        addLiveFieldRow(col, "partyAtk", tr("部队面板总攻击", "部隊総攻撃力", "Total Party ATK"))
        addLiveFieldRow(col, "partyAvailCount", tr("出击人数上限", "出撃枠数", "Deploy Count"))
        addLiveFieldRow(col, "atkPerPhys", tr("物理攻击加成%", "物理攻撃補正%", "Phys ATK%"))
        addLiveFieldRow(col, "atkPerMag", tr("魔法攻击加成%", "魔法攻撃補正%", "Mag ATK%"))
        addLiveFieldRow(col, "defPerPhys", tr("物理防御加成%", "物理防御補正%", "Phys DEF%"))
        addLiveFieldRow(col, "defPerMag", tr("魔法防御加成%", "魔法防御補正%", "Mag DEF%"))
        addLiveFieldRow(col, "perCrit", tr("暴击率加成%", "クリティカル補正%", "Crit Rate%"))
        addLiveFieldRow(col, "perMulti", tr("连击率加成%", "連撃率補正%", "Multi-Attack%"))
        addLiveFieldRow(col, "perExpUp", tr("经验获取加成%", "経験値補正%", "EXP Bonus%"))
        addLiveFieldRow(col, "statusMaxPer", tr("属性上限突破%", "上限突破%", "Status Cap%"))

        val btnApplyParty = makeSmallButton(
            tr("💾 立即写入以上部队属性修改", "💾 上記の部隊ステータスを即時反映", "💾 Apply Party Stat Edits Now"),
            "#10B981"
        ) {
            val act = JSONObject().put("partyIsFixStatus", 1)
            listOf(
                "partyHp", "partyMaxHp", "partyAtk", "partyAvailCount",
                "atkPerPhys", "atkPerMag", "defPerPhys", "defPerMag",
                "perCrit", "perMulti", "perExpUp", "statusMaxPer"
            ).forEach { key ->
                liveFields[key]?.text?.toString()?.trim()?.toIntOrNull()?.let { act.put(key, it) }
            }
            sendLiveAction(act, tr("部队属性修改已实时生效！", "部隊ステータスを反映しました！", "Party stats updated!"))
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(8)
            }
        }
        col.addView(btnApplyParty)
        return col
    }

    private fun buildPanelTab4World(): View {
        val col = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }

        val btnWorldPeace = makeSmallButton(
            tr("🌍 一键人类零伤亡 + 四国满兵力(999999) + 魔军削弱(100) + 兰斯舰队满战力", "🌍 人類被害ゼロ＋4大国兵力最大＋魔軍弱体化を即時反映", "🌍 Instant Zero Casualties + Max Human Armies + Weak Monster Armies"),
            "#059669"
        ) {
            val act = JSONObject()
                .put("humanCount", 300000)
                .put("humanDead", 0)
                .put("humanAlivePer", 100)
                .put("humanDeadPer", 0)
                .put("seaPlayerArmy", 999999)
                .put("seaEnemyArmy", 100)
                .put("leazasState", 0)
                .put("helmanState", 0)
                .put("zethState", 0)
                .put("freeCitiesState", 0)
                .put("leazasPArmy", 999999)
                .put("helmanPArmy", 999999)
                .put("zethPArmy", 999999)
                .put("freeCitiesPArmy", 999999)
                .put("leazasEArmy", 100)
                .put("helmanEArmy", 100)
                .put("zethEArmy", 100)
                .put("freeCitiesEArmy", 100)
                .put("subRemnant", 999999)
                .put("subDwarf", 999999)
                .put("subGeneral", 999999)
                .put("subHanny", 999999)
            sendLiveAction(act, tr("世界人口、四国兵力与海战战力已全部优化！", "世界情勢・各国兵力を即時反映しました！", "World population & country armies updated!"))
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = dp(8)
            }
        }
        col.addView(btnWorldPeace)

        addLiveFieldRow(col, "humanCount", tr("人类存活人口 (tt[33])", "人類生存人口", "Human Surviving Pop"))
        addLiveFieldRow(col, "humanDead", tr("人类死亡人口 (tt[34])", "人類死亡人口", "Human Casualties"))
        addLiveFieldRow(col, "seaPlayerArmy", tr("兰斯舰队战力 (tt[66])", "ランス艦隊戦力", "Rance Fleet Power"))
        addLiveFieldRow(col, "seaEnemyArmy", tr("海上魔物战力 (tt[68])", "海域魔物戦力", "Sea Monster Power"))
        addLiveFieldRow(col, "leazasPArmy", tr("利萨斯人类军 (tt[25])", "リーザス人類軍", "Leazas Human Army"))
        addLiveFieldRow(col, "leazasEArmy", tr("利萨斯魔军 (tt[29])", "リーザス魔軍", "Leazas Monster Army"))
        addLiveFieldRow(col, "helmanPArmy", tr("赫尔曼人类军 (tt[26])", "ヘルマン人類軍", "Helman Human Army"))
        addLiveFieldRow(col, "helmanEArmy", tr("赫尔曼魔军 (tt[30])", "ヘルマン魔軍", "Helman Monster Army"))
        addLiveFieldRow(col, "zethPArmy", tr("赛斯人类军 (tt[27])", "ゼス人類軍", "Zeth Human Army"))
        addLiveFieldRow(col, "zethEArmy", tr("赛斯魔军 (tt[31])", "ゼス魔軍", "Zeth Monster Army"))
        addLiveFieldRow(col, "freeCitiesPArmy", tr("自由都市人类军 (tt[28])", "自由都市人類軍", "Free Cities Human Army"))
        addLiveFieldRow(col, "freeCitiesEArmy", tr("自由都市魔军 (tt[32])", "自由都市魔軍", "Free Cities Monster Army"))

        val btnApplyWorld = makeSmallButton(
            tr("💾 立即写入以上世界局势与兵力修改", "💾 上記の世界情勢・兵力を即時反映", "💾 Apply World & Army Edits Now"),
            "#10B981"
        ) {
            applySelectedLiveFields(
                listOf(
                    "humanCount", "humanDead", "seaPlayerArmy", "seaEnemyArmy",
                    "leazasPArmy", "leazasEArmy", "helmanPArmy", "helmanEArmy",
                    "zethPArmy", "zethEArmy", "freeCitiesPArmy", "freeCitiesEArmy"
                )
            )
        }.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(8)
            }
        }
        col.addView(btnApplyWorld)
        return col
    }

    private fun updateLocksFromOverlayUI() {
        var mask = 0
        for ((bit, cb) in lockCheckBoxes) {
            if (cb.isChecked) mask = mask or bit
        }
        GameModifierBridge.setSavedLockMask(activity, mask)
    }

    private fun syncLocksToCheckboxes() {
        val mask = try {
            GameModifierBridge.nativeGetLiveLocks()
        } catch (_: Throwable) {
            GameModifierBridge.getSavedLockMask(activity)
        }
        for ((bit, cb) in lockCheckBoxes) {
            cb.setOnCheckedChangeListener(null)
            cb.isChecked = (mask and bit) != 0
            cb.setOnCheckedChangeListener { _, _ ->
                updateLocksFromOverlayUI()
            }
        }
    }

    private fun sendLiveAction(actionJson: JSONObject, successMsg: String) {
        try {
            GameModifierBridge.nativeApplyLiveAction(actionJson.toString())
            Toast.makeText(activity, successMsg, Toast.LENGTH_SHORT).show()
            handler.postDelayed({ refreshAllLiveValuesIntoForm() }, 120L)
        } catch (t: Throwable) {
            Toast.makeText(activity, "${activity.getString(R.string.error)}: ${t.message}", Toast.LENGTH_SHORT).show()
        }
    }

    private fun applySelectedLiveFields(keys: List<String>) {
        val act = JSONObject()
        for (k in keys) {
            val v = liveFields[k]?.text?.toString()?.trim()?.toIntOrNull()
            if (v != null) act.put(k, v)
        }
        sendLiveAction(act, tr("修改已实时写入游戏内存！", "変更をリアルタイム反映しました！", "Live memory updated!"))
    }

    private fun refreshStatusBannerOnly() {
        try {
            val raw = GameModifierBridge.nativeGetLiveStateJson()
            val j = JSONObject(raw)
            if (!j.optBoolean("inGame", false)) {
                statusBanner?.text = tr("游戏引擎初始化中或位于标题画面…", "タイトル画面または初期化中…", "At title screen or initializing…")
                return
            }
            val inBattle = j.optBoolean("inBattle", false)
            val hp = j.optInt("partyHp", 0)
            val maxHp = j.optInt("partyMaxHp", 0)
            val turn = j.optInt("turn", 0)
            val ap = j.optInt("battleAp", 0)
            val combo = j.optInt("battleCombo", 0)
            val eHp = j.optInt("enemyHp", 0)
            val eMaxHp = j.optInt("enemyMaxHp", 0)

            statusBanner?.text = if (inBattle) {
                tr(
                    "⚔ 战斗进行中 | 我方HP: $hp/$maxHp | AP: $ap | 连击: ${combo}% | 敌方HP: $eHp/$eMaxHp",
                    "⚔ 戦闘中 | 味方HP: $hp/$maxHp | AP: $ap | コンボ: ${combo}% | 敵HP: $eHp/$eMaxHp",
                    "⚔ In Battle | Party HP: $hp/$maxHp | AP: $ap | Combo: ${combo}% | Enemy HP: $eHp/$eMaxHp"
                )
            } else {
                tr(
                    "🏰 战略/据点阶段 | 回合: $turn | 我方HP: $hp/$maxHp | 食券: ${j.optInt("foodTicket", 0)} | 勋章: ${j.optInt("honor", 0)}",
                    "🏰 戦略フェーズ | Turn: $turn | 味方HP: $hp/$maxHp | 食券: ${j.optInt("foodTicket", 0)} | 勲章: ${j.optInt("honor", 0)}",
                    "🏰 Strategy Phase | Turn: $turn | Party HP: $hp/$maxHp | Food: ${j.optInt("foodTicket", 0)} | Honor: ${j.optInt("honor", 0)}"
                )
            }
        } catch (_: Throwable) {
        }
    }

    private fun refreshAllLiveValuesIntoForm() {
        refreshStatusBannerOnly()
        try {
            val raw = GameModifierBridge.nativeGetLiveStateJson()
            val j = JSONObject(raw)
            if (!j.optBoolean("inGame", false)) return
            for ((k, edit) in liveFields) {
                if (j.has(k) && !edit.hasFocus()) {
                    edit.setText(j.optInt(k, 0).toString())
                }
            }
        } catch (_: Throwable) {
        }
    }

    private fun addLiveFieldRow(parent: LinearLayout, key: String, label: String) {
        val row = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(2), 0, dp(2))
        }
        val tv = TextView(activity).apply {
            text = label
            setTextColor(Color.parseColor("#E5E7EB"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.3f)
        }
        val edit = EditText(activity).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f)
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED
            setPadding(dp(8), dp(4), dp(8), dp(4))
            background = GradientDrawable().apply {
                cornerRadius = dp(5).toFloat()
                setColor(Color.parseColor("#1F2937"))
                setStroke(dp(1), Color.parseColor("#374151"))
            }
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.9f)
        }
        liveFields[key] = edit
        row.addView(tv)
        row.addView(edit)
        parent.addView(row)
    }

    private fun makeSubHeader(title: String): TextView {
        return TextView(activity).apply {
            text = title
            setTextColor(Color.parseColor("#F59E0B"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            setTypeface(typeface, Typeface.BOLD)
            setPadding(0, dp(4), 0, dp(4))
        }
    }

    private fun makeActionGridButton(label: String, colorHex: String, onClick: () -> Unit): Button {
        return Button(activity).apply {
            text = label
            isAllCaps = false
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f)
            setTypeface(typeface, Typeface.BOLD)
            setPadding(dp(6), dp(6), dp(6), dp(6))
            background = GradientDrawable().apply {
                cornerRadius = dp(6).toFloat()
                setColor(Color.parseColor(colorHex))
            }
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                setMargins(dp(3), 0, dp(3), 0)
            }
            setOnClickListener { onClick() }
        }
    }

    private fun makeSmallButton(label: String, colorHex: String, onClick: () -> Unit): Button {
        return Button(activity).apply {
            text = label
            isAllCaps = false
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            setTypeface(typeface, Typeface.BOLD)
            setPadding(dp(10), dp(6), dp(10), dp(6))
            background = GradientDrawable().apply {
                cornerRadius = dp(6).toFloat()
                setColor(Color.parseColor(colorHex))
            }
            setOnClickListener { onClick() }
        }
    }
}
