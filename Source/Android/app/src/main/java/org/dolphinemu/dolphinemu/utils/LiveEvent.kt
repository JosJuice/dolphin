// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.utils

import android.os.Handler
import android.os.Looper
import android.util.ArraySet
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.Observer

open class LiveEvent<T> {
    private val observers = ArraySet<Observer<T>>()

    fun observe(owner: LifecycleOwner, observer: Observer<T>) {
        synchronized(observers) {
            if (!observers.add(observer)) {
                throw IllegalStateException("Attempted to add the same observer twice")
            }

            owner.lifecycle.addObserver(object : DefaultLifecycleObserver {
                override fun onDestroy(owner: LifecycleOwner) {
                    observers.remove(observer)
                }
            })
        }
    }

    protected open fun trigger(t: T) {
        Handler(Looper.getMainLooper()).post {
            synchronized(observers) {
                for (observer in observers) {
                    observer.onChanged(t)
                }
            }
        }
    }
}
