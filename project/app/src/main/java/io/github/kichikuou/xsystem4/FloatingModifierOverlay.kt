package io.github.kichikuou.xsystem4

import android.app.Activity
import android.app.AlertDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.drawable.GradientDrawable
import android.text.InputType
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.util.Locale
import kotlin.math.abs

class FloatingModifierOverlay(private val activity: Activity) {

    private var rootOverlay: FrameLayout? = null
    private var floatingBtn: ImageView? = null
    private var lastCheatCode: String = ""

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

    fun attach() {
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
        floatingBtn = null
        rootOverlay = null
    }

    private fun buildFloatingButton(): ImageView {
        val sizePx = dp(40)
        val padPx = dp(6)
        val btn = ImageView(activity).apply {
            setImageResource(R.drawable.ic_launcher)
            scaleType = ImageView.ScaleType.FIT_CENTER
            setPadding(padPx, padPx, padPx, padPx)
            background = GradientDrawable().apply {
                cornerRadius = dp(20).toFloat()
                setColor(0x88000000.toInt())
                setStroke(dp(1), 0x55FFFFFF)
            }
            alpha = 0.8f
            contentDescription = tr("悬浮窗修改器", "改造", "Modifier")
        }

        val lp = FrameLayout.LayoutParams(
            sizePx,
            sizePx,
            Gravity.TOP or Gravity.START
        ).apply {
            leftMargin = dp(12)
            topMargin = dp(12)
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
                    v.alpha = 0.8f
                    if (!dragging) {
                        showMainModifierDialog()
                    }
                    true
                }
                else -> false
            }
        }
        return btn
    }

    private fun readLiveJson(): JSONObject {
        return try {
            JSONObject(GameModifierBridge.nativeGetLiveStateJson())
        } catch (_: Throwable) {
            JSONObject()
        }
    }

    private fun sendAction(act: JSONObject, toastMsg: String) {
        try {
            GameModifierBridge.nativeApplyLiveAction(act.toString())
            Toast.makeText(activity, toastMsg, Toast.LENGTH_SHORT).show()
        } catch (_: Throwable) {
        }
    }

    private fun showMainModifierDialog() {
        val j = readLiveJson()
        val inGame = j.optBoolean("inGame", false)
        val inBattle = j.optBoolean("inBattle", false)
        val savedMask = GameModifierBridge.getSavedLockMask(activity)
        val lockCount = GameModifierBridge.getLiveLockCount(activity)
        val streamerLockOff = (savedMask and GameModifierBridge.LOCK_DISABLE_STREAMER_MODE) != 0
        val liveStreamerMode = j.optInt("streamerMode", if (streamerLockOff) 0 else 1)
        val isStreamerDisabled = streamerLockOff || (j.has("streamerMode") && liveStreamerMode == 0)

        val titleText = if (!inGame) {
            tr("悬浮窗修改器", "ゲーム改造", "Floating Modifier")
        } else if (inBattle) {
            val ap = j.optInt("battleAp", 0)
            val eHp = j.optInt("enemyHp", 0)
            val pHp = j.optInt("partyHp", 0)
            tr("悬浮窗修改器 [战斗 · AP:$ap · 敌HP:$eHp · 我HP:$pHp]", "改造 [戦闘 · AP:$ap · 敵HP:$eHp]", "Modifier [Battle · AP:$ap]")
        } else {
            val turn = j.optInt("turn", 0)
            val food = j.optInt("foodTicket", 0)
            val pHp = j.optInt("partyHp", 0)
            tr("悬浮窗修改器 [回合:$turn · 食券:$food · HP:$pHp]", "改造 [T:$turn · 食券:$food · HP:$pHp]", "Modifier [Turn:$turn · HP:$pHp]")
        }

        val streamerItemLabel = if (isStreamerDisabled) {
            tr("关闭主播模式：已开启 [点击恢复主播模式]", "配信者モード無効：ON", "Disable Streamer Mode: ON")
        } else {
            tr("关闭主播模式：未开启 [点击关闭主播模式]", "配信者モード無効：OFF", "Disable Streamer Mode: OFF")
        }

        val items = arrayOf(
            tr("实时锁定开关 (已开启 $lockCount 项)…", "リアルタイムロック ($lockCount 項目)…", "Live Locks ($lockCount ON)…"),
            streamerItemLabel,
            tr("输入作弊码…", "チートコード入力…", "Enter Cheat Code…"),
            tr("战斗快捷指令…", "戦闘クイック操作…", "Battle Quick Actions…"),
            tr("一键全局强化…", "一括強化プリセット…", "One-Click Boosts…"),
            tr("修改资源与回合…", "リソース・ターン編集…", "Edit Resources & Turn…"),
            tr("修改部队与属性加成…", "部隊HP・ステータス編集…", "Edit Party & Stats…"),
            tr("修改四国兵力与人口…", "四国兵力・人口編集…", "Edit World & Armies…"),
            tr("修改卡牌叠卡与角色星级…", "カード・キャラ★編集…", "Edit Cards & Chars…"),
            tr("隐藏悬浮图标", "アイコンを非表示", "Hide Floating Icon")
        )

        AlertDialog.Builder(activity)
            .setTitle(titleText)
            .setItems(items) { _, which ->
                when (which) {
                    0 -> showLocksDialog()
                    1 -> {
                        val turnOffStreamer = !isStreamerDisabled
                        GameModifierBridge.setDisableStreamerModeEnabled(activity, turnOffStreamer)
                        sendAction(
                            JSONObject().put("streamerMode", if (turnOffStreamer) 0 else 1),
                            if (turnOffStreamer) tr("已关闭主播模式（若卡面已缓存，返回标题重进即可刷新）", "配信者モードを無効化しました", "Streamer Mode disabled")
                            else tr("已重新开启主播模式", "配信者モードを有効化しました", "Streamer Mode enabled")
                        )
                    }
                    2 -> showCheatCodeDialog()
                    3 -> showBattleQuickDialog()
                    4 -> showOneClickBoostDialog()
                    5 -> showEditResourcesDialog(j)
                    6 -> showEditPartyDialog(j)
                    7 -> showEditWorldDialog(j)
                    8 -> showEditCardsDialog()
                    9 -> {
                        floatingBtn?.visibility = View.GONE
                        Toast.makeText(activity, tr("已隐藏悬浮图标", "非表示にしました", "Hidden"), Toast.LENGTH_SHORT).show()
                    }
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showLocksDialog() {
        val lockDefs = listOf(
            GameModifierBridge.LOCK_PARTY_MAX_HP to tr("锁定我方满生命", "味方HP最大ロック", "Lock Party Max HP"),
            GameModifierBridge.LOCK_INFINITE_AP to tr("锁定无限AP (99)", "AP 99ロック", "Lock Infinite AP (99)"),
            GameModifierBridge.LOCK_BATTLE_COMBO to tr("锁定999连击加成", "999コンボロック", "Lock 999 Combo"),
            GameModifierBridge.LOCK_BATTLE_ROUND_1 to tr("锁定战斗回合为1", "戦闘ラウンド1固定", "Lock Round to 1"),
            GameModifierBridge.LOCK_ONE_HIT_KILL to tr("敌方1血秒杀", "敵HP1（一撃撃破）", "One-Hit Kill (Enemy HP=1)"),
            GameModifierBridge.LOCK_ENEMY_ZERO_ATK to tr("敌方0攻击0防御", "敵の攻防0化", "Enemy 0 ATK/DEF"),
            GameModifierBridge.LOCK_LEADER_BOOST_CRIT to tr("全员蓄力必暴连击", "全員ブースト・必殺", "All Leaders Boost & Crit"),
            GameModifierBridge.LOCK_INFINITE_SKILL_USE to tr("技能无限使用次数", "スキル回数無限", "Infinite Skill Uses"),
            GameModifierBridge.LOCK_INFINITE_ZIPPO to tr("无限悔棋打火机", "やり直しジッポ無限", "Infinite Retry Zippo"),
            GameModifierBridge.LOCK_MAX_RESOURCES to tr("资源不低于999", "チケット・勲章999維持", "Keep Resources >= 999"),
            GameModifierBridge.LOCK_TREASURE_100 to tr("宝箱100%掉落", "宝箱100%ドロップ", "100% Treasure Drop")
        )
        var mask = GameModifierBridge.getSavedLockMask(activity) and GameModifierBridge.LOCK_DISABLE_STREAMER_MODE.inv()
        val labels = lockDefs.map { it.second }.toTypedArray()
        val checked = BooleanArray(lockDefs.size) { idx -> (mask and lockDefs[idx].first) != 0 }

        AlertDialog.Builder(activity)
            .setTitle(tr("实时锁定开关", "リアルタイムロック", "Live Locks"))
            .setMultiChoiceItems(labels, checked) { _, which, isChecked ->
                val bit = lockDefs[which].first
                mask = if (isChecked) (mask or bit) else (mask and bit.inv())
            }
            .setPositiveButton(R.string.ok) { _, _ ->
                GameModifierBridge.setLiveLocksOnlyMask(activity, mask)
                Toast.makeText(activity, tr("已更新锁定项", "ロックを更新しました", "Locks updated"), Toast.LENGTH_SHORT).show()
            }
            .setNeutralButton(tr("全部关闭", "全解除", "Clear All")) { _, _ ->
                GameModifierBridge.setLiveLocksOnlyMask(activity, 0)
                Toast.makeText(activity, tr("已关闭全部锁定", "全解除しました", "All locks cleared"), Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showCheatCodeDialog(prefill: String = lastCheatCode) {
        val container = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(12), dp(20), dp(6))
        }
        val tip = TextView(activity).apply {
            text = tr(
                "输入后自动呼出MOD作弊器并提交生效（无需外接键盘按0，不含双引号）：",
                "入力すると自動的にMODチートを実行します：",
                "Automatically opens MOD cheat input and submits:"
            )
            setPadding(0, 0, 0, dp(8))
        }
        val et = EditText(activity).apply {
            inputType = InputType.TYPE_CLASS_TEXT
            hint = tr("例如：whosyourdaddy / 打败哈尼王 / 餐券10", "例: whosyourdaddy", "e.g. whosyourdaddy")
            setSingleLine(true)
            setText(prefill)
            setSelection(text.length)
        }
        container.addView(tip)
        container.addView(et)

        AlertDialog.Builder(activity)
            .setTitle(tr("输入作弊码 (日不落MOD)", "チートコード入力 (MOD)", "Enter Cheat Code (MOD)"))
            .setView(container)
            .setPositiveButton(tr("执行", "実行", "Apply")) { _, _ ->
                val code = et.text.toString().trim().trim('"', '“', '”')
                if (code.isEmpty()) {
                    Toast.makeText(
                        activity,
                        tr("请输入作弊码", "コードを入力してください", "Please enter a cheat code"),
                        Toast.LENGTH_SHORT
                    ).show()
                } else {
                    lastCheatCode = code
                    if (code == "灌注Eins陈谢谢喵" || code == "关注Eins陈谢谢喵") {
                        GameModifierBridge.setDisableStreamerModeEnabled(activity, true)
                    }
                    sendAction(
                        JSONObject().put("cheatCode", code),
                        tr("已提交作弊码：$code", "チート送信：$code", "Cheat submitted: $code")
                    )
                }
            }
            .setNeutralButton(tr("常用作弊码…", "定型コード…", "Presets…")) { _, _ ->
                showCheatCodePresetsDialog()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showCheatCodePresetsDialog() {
        val presets = listOf(
            "灌注Eins陈谢谢喵" to tr("关闭主播模式口令 (灌注Eins陈谢谢喵)", "配信者モード解除パス", "Disable Streamer Mode Passphrase"),
            "1123029976" to tr("解禁全部作弊码限制 (1123029976)", "全チート制限解除 (1123029976)", "Unlock All Cheat Restrictions (1123029976)"),
            "whosyourdaddy" to tr("击杀当前敌人 (whosyourdaddy)", "敵を即撃破 (whosyourdaddy)", "Kill Enemy (whosyourdaddy)"),
            "血量全满" to tr("战斗中我方血量全满 (血量全满)", "戦闘中の味方HP全回復", "Full Party HP in Battle (血量全满)"),
            "重开" to tr("重新开始当前战斗 (重开)", "現在の戦闘をやり直す", "Restart Battle (重开)"),
            "打败哈尼王" to tr("开启兰斯世界模式 (打败哈尼王)", "ランスワールド解放", "Unlock Rance World (打败哈尼王)"),
            "111" to tr("遗物图鉴全展示 (111)", "遺物図鑑全表示 (111)", "Show All Relics (111)"),
            "panzer" to tr("获得游戏管理者卡牌 (panzer)", "ゲーム管理者獲得 (panzer)", "Get Game Admin Card (panzer)"),
            "餐券10" to tr("增加10张餐券 (餐券10，可改数字)", "食券+10 (数字変更可)", "Add 10 Food Tickets (餐券10)"),
            "部队点数up10" to tr("增加10点部队点数 (部队点数up10，可改数字)", "部隊P+10 (数字変更可)", "Add 10 Party Points (部队点数up10)"),
            "exp道具" to tr("获得1个EXP道具 (exp道具)", "EXPアイテム獲得", "Get 1 EXP Item (exp道具)"),
            "难度up4" to tr("难度+4 (难度up4，可改负数降难度)", "難易度+4 (負数で低下)", "Difficulty +4 (难度up4)"),
            "4000flag200" to tr("等级上限额外+200 (4000flag200)", "Lv上限+200 (4000flag200)", "Level Cap +200 (4000flag200)"),
            "1505flag1" to tr("开启装备解封模式 (1505flag1)", "装備解放モードON (1505flag1)", "Unlock Equipment Mode (1505flag1)"),
            "1522flag1" to tr("重置任务技能状态 (1522flag1)", "クエストスキル回数リセット", "Reset Quest Skills (1522flag1)"),
            "2222flag0" to tr("清空敌方盾与复生 (2222flag0)", "敵の盾・蘇生解除 (2222flag0)", "Clear Enemy Shield/Revive (2222flag0)"),
            "2233flag0" to tr("清空敌方自动恢复次数 (2233flag0)", "敵の自動回復解除 (2233flag0)", "Clear Enemy Auto-Heal (2233flag0)"),
            "Lv42 兰斯卡牌追加" to tr("追加指定卡牌模板 (Lv42 兰斯卡牌追加)", "指定カード追加テンプレート", "Add Card Template (Lv42 兰斯卡牌追加)"),
            "技能识别" to tr("识别正中间角色技能编号 (技能识别)", "中央キャラのスキルID確認", "Identify Center Leader Skills (技能识别)"),
            "装备识别" to tr("识别当前武器与防具代码 (装备识别)", "現在装備のID確認", "Identify Current Equipment IDs (装备识别)"),
            "梗概关闭" to tr("关闭梗概模式 (梗概关闭)", "ダイジェストモードOFF", "Disable Summary Mode (梗概关闭)")
        )
        val labels = presets.map { it.second }.toTypedArray()
        AlertDialog.Builder(activity)
            .setTitle(tr("选择常用作弊码", "定型チートコード選択", "Cheat Code Presets"))
            .setItems(labels) { _, which ->
                val chosen = presets[which].first
                lastCheatCode = chosen
                showCheatCodeDialog(chosen)
            }
            .setNegativeButton(R.string.cancel) { _, _ ->
                showCheatCodeDialog()
            }
            .show()
    }

    private fun showBattleQuickDialog() {
        val items = arrayOf(
            tr("敌方HP设为1 (秒杀)", "敵HPを1にする", "Set Enemy HP = 1"),
            tr("废除敌方攻击与防御", "敵の攻防を0にする", "Set Enemy ATK/DEF = 0"),
            tr("我方生命回满", "味方HP全回復", "Refill Party HP"),
            tr("AP设为99 + 连击999 + 回合1", "AP99＋999コンボ＋R1", "AP 99 + Combo 999 + Round 1"),
            tr("全员蓄力必暴 + 重置技能次数", "全員ブースト＋スキル回数回復", "Boost Leaders & Reset Skills"),
            tr("宝箱100%掉落", "宝箱100%ドロップ化", "100% Treasure Drop")
        )
        AlertDialog.Builder(activity)
            .setTitle(tr("战斗快捷指令", "戦闘クイック操作", "Battle Quick Actions"))
            .setItems(items) { _, which ->
                when (which) {
                    0 -> sendAction(JSONObject().put("killEnemy", true), tr("敌方HP已设为1", "敵HPを1にしました", "Enemy HP = 1"))
                    1 -> sendAction(JSONObject().put("weakenEnemy", true), tr("敌方攻防已清零", "敵攻防を0にしました", "Enemy weakened"))
                    2 -> sendAction(JSONObject().put("refillPartyHp", true), tr("我方生命已回满", "HP全回復しました", "HP refilled"))
                    3 -> sendAction(
                        JSONObject().put("battleAp", 99).put("battleCombo", 999).put("battleRound", 1),
                        tr("已设AP=99、连击=999、回合=1", "AP99・999コンボに設定", "AP=99, Combo=999, Round=1")
                    )
                    4 -> sendAction(
                        JSONObject().put("cureAndBoostLeaders", true),
                        tr("全员已蓄力必暴并重置技能", "全員強化・スキル回復完了", "Leaders boosted")
                    )
                    5 -> sendAction(
                        JSONObject().apply {
                            put("perFindTreasure", 999)
                            put("treasureNotGetCount", 99)
                            put("rareLuck", 999)
                            put("itemStar", 99)
                            put("partyIsFixStatus", 1)
                        },
                        tr("已开启宝箱100%掉落", "宝箱100%ドロップを適用", "100% Treasure Drop applied")
                    )
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showOneClickBoostDialog() {
        val items = arrayOf(
            tr("一键满资源 (食券/勋章/友情999 + 经验 + 装备+99)", "全リソース・経験値・装備★最大", "Max Resources, EXP & Item Star"),
            tr("一键宝箱100%掉落", "宝箱100%ドロップ化", "100% Treasure Drop"),
            tr("一键部队满血与全属性+999%", "部隊HP99万＋全ステータス+999%", "Max Party HP & +999% Multipliers"),
            tr("一键全卡牌99叠卡 + 全角色5星", "全カード99枚＋全キャラ★5", "All Cards x99 & All Chars 5★"),
            tr("一键四国满兵力 + 人类零伤亡", "四国兵力最大＋人類被害ゼロ", "Max Armies & Zero Casualties"),
            tr("一键关闭主播模式并填入MOD口令 (灌注Eins陈谢谢喵)", "配信者モード解除パス自動入力 (キー0)", "Auto-Fill Unlock Passphrase & Key 0"),
            tr("打开MOD内置作弊器 (按键0)", "MOD内蔵チートを開く (キー0)", "Open MOD Cheat Console (Key 0)")
        )
        AlertDialog.Builder(activity)
            .setTitle(tr("一键全局强化", "一括強化プリセット", "One-Click Boosts"))
            .setItems(items) { _, which ->
                when (which) {
                    0 -> sendAction(
                        JSONObject().apply {
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
                        },
                        tr("已生效：满资源与满经验", "全リソース最大を適用", "Max resources applied")
                    )
                    1 -> sendAction(
                        JSONObject().apply {
                            put("perFindTreasure", 999)
                            put("treasureNotGetCount", 99)
                            put("rareLuck", 999)
                            put("itemStar", 99)
                            put("partyIsFixStatus", 1)
                        },
                        tr("已生效：宝箱100%掉落", "宝箱100%ドロップを適用", "100% Treasure Drop applied")
                    )
                    2 -> sendAction(
                        JSONObject().apply {
                            put("partyHp", 999999)
                            put("partyMaxHp", 999999)
                            put("partyAvailCount", 9)
                            put("partyIsFixStatus", 1)
                            put("partyAtk", 99999)
                            put("allStatusPer", 999)
                        },
                        tr("已生效：部队满血与全属性+999%", "部隊強化を適用", "Party boosted")
                    )
                    3 -> sendAction(
                        JSONObject().apply {
                            put("setCardCount", 99)
                            put("cardAtkBonus", 999)
                            put("cardHpBonus", 999)
                            put("setCharStar", 5)
                            put("setCharExp", 999999)
                        },
                        tr("已生效：全卡牌99叠卡与全员5星", "全カード99枚・★5を適用", "Cards & chars maxed")
                    )
                    4 -> sendAction(
                        JSONObject().apply {
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
                        },
                        tr("已生效：四国满兵力与人类零伤亡", "四国兵力最大を適用", "World & armies maxed")
                    )
                    5 -> {
                        val pass = "灌注Eins陈谢谢喵"
                        try {
                            val cm = activity.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
                            cm?.setPrimaryClip(ClipData.newPlainText("mod_pass", pass))
                        } catch (_: Throwable) {
                        }
                        GameModifierBridge.setDisableStreamerModeEnabled(activity, true)
                        sendAction(
                            JSONObject()
                                .put("streamerMode", 0)
                                .put("cheatCode", pass),
                            tr("已关闭主播模式并自动提交“灌注Eins陈谢谢喵”", "パスフレーズを自動送信しました", "Passphrase auto-submitted")
                        )
                    }
                    6 -> sendAction(
                        JSONObject().put("triggerKey0", true),
                        tr("已发送按键0", "キー0を送信しました", "Sent Key 0")
                    )
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showEditResourcesDialog(j: JSONObject) {
        val fields = listOf(
            Triple("turn", tr("当前回合", "現在ターン", "Turn"), j.optInt("turn", 1)),
            Triple("difficulty", tr("难度 (0~10)", "難易度 (0~10)", "Difficulty"), j.optInt("difficulty", 1)),
            Triple("foodTicket", tr("食券数量", "食券", "Food Tickets"), j.optInt("foodTicket", 0)),
            Triple("honor", tr("勋章数量", "勲章", "Medals"), j.optInt("honor", 0)),
            Triple("friendPoint", tr("友情点数", "友好ポイント", "Friend Points"), j.optInt("friendPoint", 0)),
            Triple("totalExp", tr("累计经验值", "累計経験値", "Total EXP"), j.optInt("totalExp", 0)),
            Triple("partyRank", tr("部队等级", "部隊ランク", "Party Rank"), j.optInt("partyRank", 1)),
            Triple("itemStar", tr("装备星级加成", "装備★加算", "Item Star"), j.optInt("itemStar", 0)),
            Triple("hannyZippo", tr("悔棋打火机次数", "ジッポ回数", "Zippo Count"), j.optInt("hannyZippo", 0))
        )
        showNumberInputDialog(tr("修改资源与回合", "リソース・ターン", "Edit Resources"), fields)
    }

    private fun showEditPartyDialog(j: JSONObject) {
        val fields = listOf(
            Triple("partyHp", tr("部队当前生命", "現在HP", "Party HP"), j.optInt("partyHp", 1000)),
            Triple("partyMaxHp", tr("部队最大生命", "最大HP", "Party Max HP"), j.optInt("partyMaxHp", 1000)),
            Triple("partyAvailCount", tr("每回合出击人数", "出撃枠数", "Deploy Slots"), j.optInt("partyAvailCount", 4)),
            Triple("partyIsFixStatus", tr("锁定属性不被重算(0/1)", "ステータス固定(0/1)", "Lock Stats (0/1)"), j.optInt("partyIsFixStatus", 0)),
            Triple("partyAtk", tr("部队总攻击力", "総攻撃力", "Total ATK"), j.optInt("partyAtk", 500)),
            Triple("atkPerPhys", tr("物理攻击加成%", "物理攻撃%", "Phys ATK %"), j.optInt("atkPerPhys", 0)),
            Triple("atkPerMag", tr("魔法攻击加成%", "魔法攻撃%", "Mag ATK %"), j.optInt("atkPerMag", 0)),
            Triple("defPerPhys", tr("物理防御加成%", "物理防御%", "Phys DEF %"), j.optInt("defPerPhys", 0)),
            Triple("defPerMag", tr("魔法防御加成%", "魔法防御%", "Mag DEF %"), j.optInt("defPerMag", 0)),
            Triple("perCrit", tr("暴击率加成%", "クリティカル%", "Crit %"), j.optInt("perCrit", 0)),
            Triple("perMulti", tr("连击率加成%", "連撃率%", "Multi-Hit %"), j.optInt("perMulti", 0)),
            Triple("perExpUp", tr("经验加成%", "経験値%", "EXP %"), j.optInt("perExpUp", 0)),
            Triple("perFindTreasure", tr("宝箱掉落率%", "宝箱発見率%", "Treasure Drop %"), j.optInt("perFindTreasure", 0))
        )
        showNumberInputDialog(tr("修改部队与属性加成", "部隊・ステータス", "Edit Party & Stats"), fields)
    }

    private fun showEditWorldDialog(j: JSONObject) {
        val fields = listOf(
            Triple("humanCount", tr("人类存活人口", "人類生存人口", "Human Alive"), j.optInt("humanCount", 300000)),
            Triple("humanDead", tr("人类死亡人口", "人類死亡人口", "Human Dead"), j.optInt("humanDead", 0)),
            Triple("seaPlayerArmy", tr("兰斯船团兵力", "ランス船団兵力", "Sea Fleet Army"), j.optInt("seaPlayerArmy", 0)),
            Triple("leazasPArmy", tr("利萨斯我军兵力", "リーザス味方兵力", "Leazas Player Army"), j.optInt("leazasPArmy", 300)),
            Triple("helmanPArmy", tr("赫尔曼我军兵力", "ヘルマン味方兵力", "Helman Player Army"), j.optInt("helmanPArmy", 300)),
            Triple("zethPArmy", tr("赛斯我军兵力", "ゼス味方兵力", "Zeth Player Army"), j.optInt("zethPArmy", 300)),
            Triple("freeCitiesPArmy", tr("自由都市我军兵力", "自由都市味方兵力", "Free Cities Player Army"), j.optInt("freeCitiesPArmy", 300)),
            Triple("leazasEArmy", tr("利萨斯魔军兵力", "リーザス魔軍兵力", "Leazas Enemy Army"), j.optInt("leazasEArmy", 500)),
            Triple("helmanEArmy", tr("赫尔曼魔军兵力", "ヘルマン魔軍兵力", "Helman Enemy Army"), j.optInt("helmanEArmy", 500)),
            Triple("zethEArmy", tr("赛斯魔军兵力", "ゼス魔軍兵力", "Zeth Enemy Army"), j.optInt("zethEArmy", 500)),
            Triple("freeCitiesEArmy", tr("自由都市魔军兵力", "自由都市魔軍兵力", "Free Cities Enemy Army"), j.optInt("freeCitiesEArmy", 500))
        )
        showNumberInputDialog(tr("修改四国兵力与人口", "四国・人口", "Edit World & Armies"), fields)
    }

    private fun showEditCardsDialog() {
        val fields = listOf(
            Triple("setCardCount", tr("全卡牌叠卡设为N张", "全カード枚数指定", "Set All Cards Count"), 99),
            Triple("cardAtkBonus", tr("全卡牌攻击加成", "全カード攻撃加算", "All Cards ATK Bonus"), 999),
            Triple("cardHpBonus", tr("全卡牌生命加成", "全カードHP加算", "All Cards HP Bonus"), 999),
            Triple("setCharStar", tr("全角色星级 (1~5)", "全キャラ★ (1~5)", "All Chars Star (1~5)"), 5),
            Triple("setCharExp", tr("全角色经验值", "全キャラ経験値", "All Chars EXP"), 999999)
        )
        showNumberInputDialog(tr("修改卡牌与角色", "カード・キャラ", "Edit Cards & Chars"), fields)
    }

    private fun showNumberInputDialog(
        title: String,
        fields: List<Triple<String, String, Int>>
    ) {
        val scroll = ScrollView(activity)
        val container = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(10), dp(20), dp(6))
        }
        val inputs = LinkedHashMap<String, EditText>()
        for ((key, label, defVal) in fields) {
            val row = LinearLayout(activity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(0, dp(3), 0, dp(3))
            }
            val tv = TextView(activity).apply {
                text = label
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.2f)
            }
            val et = EditText(activity).apply {
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

        AlertDialog.Builder(activity)
            .setTitle(title)
            .setView(scroll)
            .setPositiveButton(R.string.ok) { _, _ ->
                val act = JSONObject()
                for ((key, et) in inputs) {
                    val v = et.text.toString().trim().toIntOrNull()
                    if (v != null) act.put(key, v)
                }
                sendAction(act, tr("修改已生效", "適用しました", "Applied"))
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }
}
