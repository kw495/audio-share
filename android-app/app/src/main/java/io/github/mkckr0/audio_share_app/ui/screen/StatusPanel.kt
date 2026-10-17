/*
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

package io.github.mkckr0.audio_share_app.ui.screen

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import io.github.mkckr0.audio_share_app.R
import io.github.mkckr0.audio_share_app.model.AudioUsageValues
import io.github.mkckr0.audio_share_app.service.PlaybackStatus
import kotlinx.coroutines.delay
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Connection state, received format and receive statistics. */
@Composable
fun StatusCard(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val state = PlaybackStatus.state
    val format = PlaybackStatus.format
    val stats = PlaybackStatus.stats
    // ticks so "last packet" keeps counting while nothing arrives
    val now by produceState(System.currentTimeMillis()) {
        while (true) {
            value = System.currentTimeMillis()
            delay(1000)
        }
    }

    val (stateText, stateColor) = when (state) {
        PlaybackStatus.State.STOPPED -> context.getString(R.string.label_state_stopped) to MaterialTheme.colorScheme.outline
        PlaybackStatus.State.CONNECTING -> context.getString(R.string.label_state_connecting) to Color(0xFFF9A825)
        PlaybackStatus.State.SILENT -> context.getString(R.string.label_state_silent) to MaterialTheme.colorScheme.primary
        PlaybackStatus.State.RECEIVING -> context.getString(R.string.label_state_receiving) to Color(0xFF2E7D32)
        PlaybackStatus.State.RETRYING -> context.getString(R.string.label_state_retrying) to MaterialTheme.colorScheme.error
    }
    val none = "—"

    OutlinedCard(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(10.dp)
                        .background(stateColor, CircleShape)
                )
                Spacer(Modifier.width(8.dp))
                Text(stateText, style = MaterialTheme.typography.titleMedium)
            }
            StatusRow(context.getString(R.string.label_server), PlaybackStatus.server.ifEmpty { none })
            StatusRow(
                context.getString(R.string.label_format),
                format?.let { PlaybackStatus.describe(context, it) } ?: none
            )
            StatusRow(
                context.getString(R.string.label_audio_usage),
                format?.let { usageLabel(it.usage) } ?: none
            )
            StatusRow(
                context.getString(R.string.label_receive_rate),
                if (state == PlaybackStatus.State.STOPPED || format == null) none
                else "${PlaybackStatus.formatBytes(stats.bytesPerSecond)}/s · " +
                    context.getString(R.string.label_packets_per_second, stats.packetsPerSecond.toInt())
            )
            StatusRow(
                context.getString(R.string.label_total_received),
                if (stats.totalPackets == 0L) none
                else "${PlaybackStatus.formatBytes(stats.totalBytes)} · " +
                    context.getString(R.string.label_packets, stats.totalPackets.toInt())
            )
            StatusRow(
                context.getString(R.string.label_last_packet),
                if (stats.lastPacketAtMillis == 0L) none
                else context.getString(
                    R.string.label_seconds_ago,
                    (now - stats.lastPacketAtMillis).coerceAtLeast(0) / 1000.0
                )
            )
        }
    }
}

@Composable
private fun StatusRow(label: String, value: String) {
    Row(modifier = Modifier.fillMaxWidth()) {
        Text(
            label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.width(96.dp),
        )
        Text(value, style = MaterialTheme.typography.bodyMedium)
    }
}

@Composable
private fun usageLabel(usage: String): String {
    val context = LocalContext.current
    return context.getString(
        when (usage) {
            AudioUsageValues.ALARM -> R.string.label_audio_usage_alarm
            AudioUsageValues.NOTIFICATION -> R.string.label_audio_usage_notification
            AudioUsageValues.RINGTONE -> R.string.label_audio_usage_ringtone
            else -> R.string.label_audio_usage_media
        }
    )
}

/** Recent events, newest at the bottom; follows new entries. */
@Composable
fun LogCard(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val logs = PlaybackStatus.logs
    val listState = rememberLazyListState()
    val timeFormat = remember { SimpleDateFormat("HH:mm:ss", Locale.getDefault()) }

    LaunchedEffect(logs.size) {
        if (logs.isNotEmpty()) {
            listState.animateScrollToItem(logs.size - 1)
        }
    }

    OutlinedCard(modifier = modifier.fillMaxWidth()) {
        Column(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = 16.dp, end = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    context.getString(R.string.label_log),
                    style = MaterialTheme.typography.titleSmall,
                    modifier = Modifier.weight(1f),
                )
                TextButton(onClick = { PlaybackStatus.clearLog() }) {
                    Text(context.getString(R.string.label_clear))
                }
            }
            SelectionContainer(modifier = Modifier.weight(1f)) {
                LazyColumn(
                    state = listState,
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(start = 16.dp, end = 16.dp, bottom = 12.dp),
                ) {
                    items(logs) { entry ->
                        Text(
                            "${timeFormat.format(Date(entry.timeMillis))}  ${entry.message}",
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                        )
                    }
                }
            }
        }
    }
}
