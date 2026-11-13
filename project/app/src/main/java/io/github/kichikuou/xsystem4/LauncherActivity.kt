package io.github.kichikuou.xsystem4

import android.app.Activity
import android.app.AlertDialog
import android.app.Dialog
import android.content.Context
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.LayoutInflater
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.BaseAdapter
import android.widget.ImageView
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast

private const val ICON_SIZE_DP = 40
private const val INSTALL_REQUEST = 1
private const val DIRECTORY_REQUEST = 2
private const val MOD_ZIP_REQUEST = 3
private const val MOD_DIRECTORY_REQUEST = 4
private const val GITHUB_URL = "https://github.com/TomorrowX6/re-xsystem4"

private class GameListAdapter(activity: Activity, private val gameList: GameList) : BaseAdapter() {
    companion object {
        private var defaultTextColor: ColorStateList? = null
    }
    private val context: Context = activity

    override fun getCount(): Int = gameList.size
    override fun getItem(position: Int): Any = gameList[position]
    override fun getItemId(position: Int): Long = position.toLong()

    override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
        val item = gameList[position]
        val view = convertView
            ?: LayoutInflater.from(context).inflate(R.layout.launcher_item, parent, false)
        val icon = item.getIconBitmap((ICON_SIZE_DP * context.resources.displayMetrics.density).toInt())
        if (icon != null) view.findViewById<ImageView>(R.id.icon).setImageBitmap(icon)
        else view.findViewById<ImageView>(R.id.icon).setImageResource(R.drawable.ic_launcher)
        val title = view.findViewById<TextView>(R.id.title)
        if (defaultTextColor == null)
            defaultTextColor = title.textColors
        title.text = item.name
        if (item.error != null)
            title.setTextColor(Color.GRAY)
        else
            title.setTextColor(defaultTextColor)
        return view
    }
}

class LauncherActivity : Activity(), GameListObserver {
    companion object {
        private var savedGameList: GameList? = null
    }

    private lateinit var gameList: GameList
    private lateinit var adapter: GameListAdapter
    private var progressDialog: Dialog? = null
    private var pendingModTarget: Item? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.launcher)

        gameList = getGameList(refresh = false)
        gameList.observer = this
        adapter = GameListAdapter(this, gameList)

        val listView = findViewById<ListView>(R.id.list)
        listView.adapter = adapter
        findViewById<View>(R.id.install_game).setOnClickListener { openInstallGame() }
        findViewById<View>(R.id.install_mod).setOnClickListener { openInstallMod() }
        findViewById<View>(R.id.save_manager).setOnClickListener { openSaveManager() }
        findViewById<View>(R.id.game_modifier).setOnClickListener { openGameModifier() }
        findViewById<View>(R.id.open_github).setOnClickListener { openGitHub() }
        listView.setOnItemClickListener { _, _, pos, _ ->
            onListItemClick(listView.adapter.getItem(pos) as Item)
        }
        listView.setOnItemLongClickListener { _, _, pos, _ ->
            onItemLongClick(listView.adapter.getItem(pos) as Item)
        }
        renderInstallState(gameList.installState)
    }

    override fun onDestroy() {
        if (gameList.observer === this) {
            gameList.observer = null
        }
        dismissProgressDialog()
        super.onDestroy()
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.launcher_menu, menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        return when (item.itemId) {
            R.id.refresh -> {
                setGameList(getGameList(refresh = true))
                true
            }
            R.id.install_game -> {
                openInstallGame()
                true
            }
            R.id.install_mod -> {
                openInstallMod()
                true
            }
            R.id.save_manager -> {
                openSaveManager()
                true
            }
            R.id.game_modifier -> {
                openGameModifier()
                true
            }
            R.id.licenses -> {
                val intent = Intent(this, LicensesMenuActivity::class.java)
                startActivity(intent)
                true
            }
            else -> super.onOptionsItemSelected(item)
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK)
            return
        val uri = data?.data ?: return
        when (requestCode) {
            DIRECTORY_REQUEST -> {
                gameList.installDirectory(uri)
                renderInstallState(gameList.installState)
            }
            INSTALL_REQUEST -> {
                gameList.installArchive(uri)
                renderInstallState(gameList.installState)
            }
            MOD_DIRECTORY_REQUEST -> {
                val target = pendingModTarget ?: if (gameList.size > 0) gameList[0] else return
                gameList.installModDirectory(target, uri)
                renderInstallState(gameList.installState)
            }
            MOD_ZIP_REQUEST -> {
                val target = pendingModTarget ?: if (gameList.size > 0) gameList[0] else return
                gameList.installModArchive(target, uri)
                renderInstallState(gameList.installState)
            }
        }
    }

    private fun openInstallGame() {
        if (gameList.installState is InstallState.Installing) return
        val options = arrayOf(
            getString(R.string.game_opt_zip),
            getString(R.string.game_opt_dir)
        )
        AlertDialog.Builder(this)
            .setTitle(R.string.action_install_game)
            .setItems(options) { _, which ->
                when (which) {
                    0 -> chooseZip()
                    1 -> chooseDirectory()
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun openInstallMod() {
        if (gameList.installState is InstallState.Installing) return
        if (gameList.size == 0) {
            AlertDialog.Builder(this)
                .setTitle(R.string.action_install_mod)
                .setMessage(R.string.mod_no_game_installed)
                .setPositiveButton(R.string.ok, null)
                .show()
            return
        }
        if (gameList.size == 1) {
            showModActionDialog(gameList[0])
            return
        }
        val names = Array(gameList.size) { gameList[it].name }
        AlertDialog.Builder(this)
            .setTitle(R.string.mod_select_game_title)
            .setItems(names) { _, which ->
                if (which in 0 until gameList.size) {
                    showModActionDialog(gameList[which])
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showModActionDialog(target: Item) {
        val options = mutableListOf(
            getString(R.string.mod_opt_zip),
            getString(R.string.mod_opt_dir)
        )
        if (gameList.hasInstalledMod(target)) {
            options.add(getString(R.string.mod_opt_restore_vanilla))
        }
        AlertDialog.Builder(this)
            .setTitle(getString(R.string.mod_dialog_title, target.name))
            .setItems(options.toTypedArray()) { _, which ->
                when (which) {
                    0 -> {
                        pendingModTarget = target
                        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                            addCategory(Intent.CATEGORY_OPENABLE)
                            type = "*/*"
                            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        }
                        startActivityForResult(Intent.createChooser(i, getString(R.string.mod_choose_zip)), MOD_ZIP_REQUEST)
                    }
                    1 -> {
                        pendingModTarget = target
                        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
                            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        }, MOD_DIRECTORY_REQUEST)
                    }
                    2 -> {
                        if (gameList.restoreMod(target)) {
                            adapter.notifyDataSetChanged()
                            Toast.makeText(this, R.string.mod_restored_toast, Toast.LENGTH_SHORT).show()
                        }
                    }
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun chooseDirectory() {
        if (gameList.installState is InstallState.Installing) return
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }, DIRECTORY_REQUEST)
    }

    private fun chooseZip() {
        if (gameList.installState is InstallState.Installing) return
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivityForResult(Intent.createChooser(i, getString(R.string.choose_a_file)), INSTALL_REQUEST)
    }

    private fun openSaveManager(dirName: String? = null) {
        val intent = Intent(this, SaveManagerActivity::class.java)
        if (dirName != null) {
            intent.putExtra(SaveManagerActivity.EXTRA_DIR_NAME, dirName)
        }
        startActivity(intent)
    }

    private fun openGameModifier(dirName: String? = null) {
        val intent = Intent(this, GameModifierActivity::class.java)
        if (dirName != null) {
            intent.putExtra(GameModifierActivity.EXTRA_DIR_NAME, dirName)
        }
        startActivity(intent)
    }

    private fun openGitHub() {
        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(GITHUB_URL)))
    }

    override fun onInstallProgress(path: String) {
        renderInstallState(gameList.installState)
    }

    override fun onInstallSuccess() {
        renderInstallState(gameList.installState)
    }

    override fun onInstallFailure(msgId: Int) {
        renderInstallState(gameList.installState)
    }

    private fun getGameList(refresh: Boolean): GameList {
        if (savedGameList == null || refresh && savedGameList?.installState is InstallState.Idle) {
            savedGameList = GameList(this)
        }
        return savedGameList!!
    }

    private fun setGameList(newGameList: GameList) {
        if (gameList.observer === this) {
            gameList.observer = null
        }
        gameList = newGameList
        gameList.observer = this
        adapter = GameListAdapter(this, gameList)
        findViewById<ListView>(R.id.list).adapter = adapter
        renderInstallState(gameList.installState)
    }

    private fun onListItemClick(item: Item) {
        if (item.error != null) {
            AlertDialog.Builder(this)
                .setTitle(R.string.error)
                .setMessage(item.error)
                .setPositiveButton(R.string.ok, null)
                .show()
            return
        }
        val i = Intent()
        i.setClass(applicationContext, XSystem4Activity::class.java)
        i.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        i.putExtra(XSystem4Activity.EXTRA_GAME_ROOT, item.path.path)
        i.putExtra(XSystem4Activity.EXTRA_SAVE_DIR, item.savedir!!.path)
        startActivity(i)
    }

    private fun onItemLongClick(item: Item): Boolean {
        AlertDialog.Builder(this).setTitle(R.string.uninstall_dialog_title)
            .setMessage(getString(R.string.uninstall_dialog_message, item.name))
            .setPositiveButton(R.string.ok) {_, _ ->
                gameList.uninstall(item)
                adapter.notifyDataSetChanged()
            }
            .setNegativeButton(R.string.cancel) {_, _ -> }
            .show()
        return true
    }

    private fun renderInstallState(state: InstallState) {
        when (state) {
            InstallState.Idle -> dismissProgressDialog()
            is InstallState.Installing -> showProgressDialog(state.progress)
            InstallState.Succeeded -> {
                dismissProgressDialog()
                adapter.notifyDataSetChanged()
                gameList.consumeInstallResult()
            }
            is InstallState.Failed -> {
                dismissProgressDialog()
                AlertDialog.Builder(this).setTitle(R.string.error)
                    .setMessage(state.msgId)
                    .setPositiveButton(R.string.ok) {_, _ -> }
                    .show()
                gameList.consumeInstallResult()
            }
        }
    }

    private fun showProgressDialog(progress: String? = null) {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (progressDialog == null) {
            progressDialog = Dialog(this)
            progressDialog!!.apply {
                setTitle(R.string.install_dialog_title)
                setCancelable(false)
                setContentView(R.layout.progress_dialog)
                show()
            }
        }
        progress?.let {
            progressDialog?.findViewById<TextView>(R.id.text)?.text = getString(R.string.install_progress, it)
        }
    }

    private fun dismissProgressDialog() {
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        progressDialog?.dismiss()
        progressDialog = null
    }
}
