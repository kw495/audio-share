/*
 *    Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>
 *    Modified 2026 by kw495 (see NOTICE)
 *
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

package io.github.mkckr0.audio_share_app.ui.base

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.floatPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import io.github.mkckr0.audio_share_app.model.audioConfigDataStore
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch

@Composable
fun ConfigGroup(title: String, content: @Composable () -> Unit) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp)
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium
            )

            Spacer(Modifier.height(8.dp))

            content()
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SliderConfig(
    key: String,
    title: String,
    valueFormatter: (value: Float) -> String,
    defaultValue: Float,
    valueRange: ClosedFloatingPointRange<Float>,
    step: Float,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    val value = remember {
        context.audioConfigDataStore.data.map {
            it[floatPreferencesKey(key)] ?: defaultValue
        }
    }.collectAsState(null).value

    if (value != null) {
        var tempValue by remember(value) { mutableFloatStateOf(value) }
        Column(
            modifier = Modifier.padding(top = 12.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(title, style = MaterialTheme.typography.bodyMedium)
                Text(valueFormatter(tempValue), style = MaterialTheme.typography.bodyMedium)
            }

            Slider(
                value = tempValue,
                onValueChange = { tempValue = it },
                onValueChangeFinished = {
                    scope.launch {
                        context.audioConfigDataStore.edit {
                            it[floatPreferencesKey(key)] = tempValue
                        }
                    }
                },
                steps = ((valueRange.endInclusive - valueRange.start) / step).toInt() - 1,
                valueRange = valueRange,
                track = {
                    SliderDefaults.Track(it, drawTick = { _, _ -> })
                },
                modifier = Modifier.padding(top = 4.dp, start = 2.dp, end = 2.dp),
            )
        }
    }
}

/**
 * A string preference picked from a fixed list. `options` maps stored value to label.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChoiceConfig(
    key: String,
    title: String,
    options: List<Pair<String, String>>,
    defaultValue: String,
    summary: String? = null,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    val value = remember {
        context.audioConfigDataStore.data.map {
            it[stringPreferencesKey(key)] ?: defaultValue
        }
    }.collectAsState(null).value

    if (value != null) {
        var expanded by remember { mutableStateOf(false) }
        Column(
            modifier = Modifier.padding(top = 12.dp)
        ) {
            Text(title, style = MaterialTheme.typography.bodyMedium)
            if (summary != null) {
                Text(
                    summary,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            ExposedDropdownMenuBox(
                expanded = expanded,
                onExpandedChange = { expanded = it },
                modifier = Modifier.padding(top = 4.dp),
            ) {
                OutlinedTextField(
                    value = options.firstOrNull { it.first == value }?.second ?: value,
                    onValueChange = {},
                    readOnly = true,
                    singleLine = true,
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) },
                    modifier = Modifier
                        .menuAnchor(MenuAnchorType.PrimaryNotEditable)
                        .fillMaxWidth(),
                )
                ExposedDropdownMenu(
                    expanded = expanded,
                    onDismissRequest = { expanded = false },
                ) {
                    options.forEach { (optionValue, label) ->
                        DropdownMenuItem(
                            text = { Text(label) },
                            onClick = {
                                expanded = false
                                scope.launch {
                                    context.audioConfigDataStore.edit {
                                        it[stringPreferencesKey(key)] = optionValue
                                    }
                                }
                            },
                        )
                    }
                }
            }
        }
    }
}