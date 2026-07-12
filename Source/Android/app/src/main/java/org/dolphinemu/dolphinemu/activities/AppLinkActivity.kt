// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.activities

import android.content.Intent
import android.os.Bundle
import android.util.Log
import androidx.fragment.app.FragmentActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import org.dolphinemu.dolphinemu.model.GameFile
import org.dolphinemu.dolphinemu.services.GameFileCacheManager
import org.dolphinemu.dolphinemu.ui.main.TvMainActivity
import org.dolphinemu.dolphinemu.utils.AppLinkHelper
import org.dolphinemu.dolphinemu.utils.AppLinkHelper.PlayAction
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization

/**
 * Linker between leanback homescreen and app
 */
class AppLinkActivity : FragmentActivity() {
    private lateinit var playAction: PlayAction
    private lateinit var tryPlayJob: Job

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val uri = intent.data!!

        Log.v(TAG, uri.toString())

        if (uri.pathSegments.isEmpty()) {
            Log.e(TAG, "Invalid uri $uri")
            finish()
            return
        }

        val action = AppLinkHelper.extractAction(uri)
        when (action.action) {
            AppLinkHelper.PLAY -> {
                playAction = action as PlayAction
                initResources()
            }

            AppLinkHelper.BROWSE -> browse()
            else -> throw IllegalArgumentException("Invalid Action $action")
        }
    }

    /**
     * Need to init these since they usually occur in the main activity.
     */
    private fun initResources() {
        tryPlayJob = lifecycleScope.launch {
            DirectoryInitialization.waitUntilInitialized()
            tryPlay(playAction)
        }

        GameFileCacheManager.isLoading().observe(this) { isLoading: Boolean? ->
            if (!isLoading!! && DirectoryInitialization.areDolphinDirectoriesReady()) {
                tryPlayJob.cancel()
                tryPlay(playAction)
            }
        }

        DirectoryInitialization.start(this)
        GameFileCacheManager.startLoad()
    }

    /**
     * Action if channel icon is selected
     */
    private fun browse() {
        val openApp = Intent(this, TvMainActivity::class.java)
        startActivity(openApp)

        finish()
    }

    private fun tryPlay(action: PlayAction) {
        // TODO: This approach of getting the game from the game file cache without rescanning the
        //       library means that we can fail to launch games if the cache file has been deleted.
        val game = GameFileCacheManager.getGameFileByGameId(action.gameId)

        // If game == null and the load isn't done, wait for the next GameFileCacheService broadcast.
        // If game == null and the load is done, call play with a null game, making us exit in failure.
        if (game != null || !GameFileCacheManager.isLoading().value!!) {
            play(action, game)
        }
    }

    /**
     * Action if program(game) is selected
     */
    private fun play(action: PlayAction, game: GameFile?) {
        Log.d(TAG, "Playing game ${action.gameId} from channel ${action.channelId}")
        game?.let { startGame(it) } ?: Log.e(TAG, "Invalid Game: " + action.gameId)
        finish()
    }

    private fun startGame(game: GameFile) {
        EmulationActivity.launch(this, GameFileCacheManager.findSecondDiscAndGetPaths(game), false)
    }

    companion object {
        private const val TAG = "AppLinkActivity"
    }
}
