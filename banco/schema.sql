-- ============================================================
-- SMART COFFEE — Núcleo de rastreabilidade
-- PostgreSQL 15+
-- Fase 1: apenas o que sustenta a trilha de auditoria.
-- Sem IoT, sem RFID, sem blockchain. Isso vem depois, em cima disto.
-- ============================================================

create extension if not exists "pgcrypto";   -- gen_random_uuid, digest
create extension if not exists "postgis";    -- geo de talhão (EUDR)

-- ------------------------------------------------------------
-- 1. IDENTIDADE E TAXONOMIAS
--    Já existem no protótipo. Só normalizadas.
-- ------------------------------------------------------------

create table usuario (
    usuario_id   uuid primary key default gen_random_uuid(),
    login        text not null unique,
    nome         text not null,
    papel        text not null check (papel in
                     ('OPERADOR','TECNICO','SUPERVISOR','ADMIN','AUDITOR')),
    ativo        boolean not null default true,
    criado_em    timestamptz not null default now()
);

create table fazenda (
    fazenda_id   uuid primary key default gen_random_uuid(),
    codigo       text not null unique,          -- 'IGR'
    nome         text not null,
    cnpj_cpf     text
);

-- Talhão. A geometria aqui é o que atende exigência de origem
-- em exportação (EUDR). Sem isso não há prova de procedência.
create table talhao (
    talhao_id    uuid primary key default gen_random_uuid(),
    fazenda_id   uuid not null references fazenda,
    codigo       text not null,                 -- 'Igr2.13'
    nome         text not null,                 -- 'Igrejinha II'
    area_ha      numeric(10,4),
    geometria    geometry(Polygon, 4326),       -- polígono do talhão
    centroide    geometry(Point, 4326),
    unique (fazenda_id, codigo)
);

-- Taxonomias auxiliares. `codigo_num` é o dígito usado no MTP.
create table variedade (
    variedade_id uuid primary key default gen_random_uuid(),
    codigo_num   int not null unique,
    nome         text not null                  -- 'Catucaí 2SL'
);

create table processo (
    processo_id  uuid primary key default gen_random_uuid(),
    codigo_num   int not null unique,
    nome         text not null                  -- 'Fermentação Espontânea / Aeróbica'
);

create table metodo_colheita (
    colheita_id  uuid primary key default gen_random_uuid(),
    codigo_num   int not null unique,
    nome         text not null                  -- 'Manual'
);

create table metodo_secagem (
    secagem_id   uuid primary key default gen_random_uuid(),
    codigo_num   int not null unique,
    nome         text not null                  -- 'Seco em Camas Suspensas'
);

-- Tulhas: 36 unidades, lado direito = terceiros, esquerdo = fazenda.
create table tulha (
    tulha_id     uuid primary key default gen_random_uuid(),
    numero       int not null unique,
    lado         text not null check (lado in ('ESQUERDO','DIREITO')),
    capacidade_kg numeric(10,2),
    dedicada     boolean not null default true  -- true = 1 lote por vez
);

-- ------------------------------------------------------------
-- 2. LOTE
--    PK opaca e imutável. O código MTP é DERIVADO, nunca chave.
--    Este é o ponto que o protótipo atual erra.
-- ------------------------------------------------------------

create table lote (
    lote_id       uuid primary key default gen_random_uuid(),

    -- Identificador humano curto e estável, gerado por sequência.
    -- Não carrega significado. Serve para conversa e etiqueta.
    referencia    text not null unique,          -- 'L-2026-000431'

    -- Código MTP: projeção legível dos atributos correntes.
    -- Recalculado a cada mudança. NÃO é identidade.
    codigo_mtp    text,

    safra         int not null,
    talhao_id     uuid not null references talhao,
    variedade_id  uuid references variedade,
    processo_id   uuid references processo,
    colheita_id   uuid references metodo_colheita,
    secagem_id    uuid references metodo_secagem,

    lavado        boolean,
    descascado    boolean,
    numero_lote   int,                           -- último dígito do MTP

    -- Máquina de estado. A API rejeita transições inválidas.
    status        text not null default 'RASCUNHO' check (status in (
                      'RASCUNHO','ABERTO','EM_PROCESSO','EM_DESCANSO',
                      'BENEFICIADO','EXPEDIDO','CANCELADO')),

    lote_pai_id   uuid references lote,          -- mini-lote -> lote grande
    peso_bruto_kg numeric(12,3),
    peso_liquido_kg numeric(12,3),

    criado_por    uuid not null references usuario,
    criado_em     timestamptz not null default now(),
    atualizado_em timestamptz not null default now()
);

create index on lote (status);
create index on lote (safra, talhao_id);
create index on lote (lote_pai_id);
create index on lote (codigo_mtp);

comment on column lote.codigo_mtp is
  'Projeção legível. Muda quando atributos mudam. Jamais usar como FK.';

-- Recalcula o MTP quando qualquer atributo componente muda.
create or replace function fn_recalcula_mtp() returns trigger as $$
declare
    v_talhao text; v_proc int; v_colh int; v_seca int;
begin
    select t.codigo into v_talhao from talhao t where t.talhao_id = new.talhao_id;
    select p.codigo_num into v_proc from processo p where p.processo_id = new.processo_id;
    select c.codigo_num into v_colh from metodo_colheita c where c.colheita_id = new.colheita_id;
    select s.codigo_num into v_seca from metodo_secagem s where s.secagem_id = new.secagem_id;

    new.codigo_mtp := concat_ws('.',
        v_talhao,
        coalesce(v_proc::text,'0'),
        coalesce(v_colh::text,'0'),
        case when new.lavado     then '1' else '0' end,
        case when new.descascado then '1' else '0' end,
        coalesce(v_seca::text,'0'),
        coalesce(new.numero_lote::text,'0')
    );
    new.atualizado_em := now();
    return new;
end $$ language plpgsql;

create trigger trg_lote_mtp before insert or update on lote
    for each row execute function fn_recalcula_mtp();

-- ------------------------------------------------------------
-- 3. EVENTO — o coração
--    Append-only. Correção = evento compensatório, nunca UPDATE.
--    Hash encadeado por lote dá detecção de adulteração sem blockchain.
-- ------------------------------------------------------------

create table evento (
    evento_id     uuid primary key default gen_random_uuid(),
    lote_id       uuid not null references lote,

    tipo          text not null check (tipo in (
                      'LOTE_CRIADO','RECEPCAO','AMOSTRAGEM','LAVAGEM',
                      'FERMENTACAO_INICIO','FERMENTACAO_FIM',
                      'SECAGEM_INICIO','SECAGEM_FIM',
                      'ENTRADA_TULHA','SAIDA_TULHA',
                      'TRANSFORMACAO','AGREGACAO','DESAGREGACAO',
                      'CLASSIFICACAO','EXPEDICAO',
                      'CORRECAO','CANCELAMENTO')),

    -- Autoria: sem isso não existe auditoria.
    usuario_id    uuid not null references usuario,

    -- Dois tempos, sempre. Divergência grande denuncia
    -- relógio errado ou sincronização atrasada de app offline.
    ts_ocorrencia timestamptz not null,
    ts_registro   timestamptz not null default now(),

    origem        text not null default 'APP'
                  check (origem in ('APP','SENSOR','RFID','BLE','IMPORT','SISTEMA')),

    -- Dados específicos do tipo. Validado por JSON Schema na API.
    payload       jsonb not null default '{}'::jsonb,

    -- Evento que este corrige (só para tipo = CORRECAO).
    corrige_id    uuid references evento,
    motivo        text,

    -- Idempotência: retry e sync offline não duplicam.
    idem_key      text not null unique,

    -- Cadeia de integridade, por lote.
    seq_lote      bigint not null,
    prev_hash     bytea,
    hash          bytea not null,

    unique (lote_id, seq_lote)
);

create index on evento (lote_id, ts_ocorrencia);
create index on evento (tipo);
create index on evento (usuario_id);
create index on evento using gin (payload);

-- Calcula seq e hash antes de inserir.
create or replace function fn_evento_hash() returns trigger as $$
declare
    v_prev bytea; v_seq bigint;
begin
    select e.hash, e.seq_lote into v_prev, v_seq
      from evento e
     where e.lote_id = new.lote_id
     order by e.seq_lote desc limit 1;

    new.seq_lote  := coalesce(v_seq, 0) + 1;
    new.prev_hash := v_prev;
    new.hash := digest(
        coalesce(encode(v_prev,'hex'),'') ||
        new.lote_id::text || new.tipo || new.usuario_id::text ||
        new.ts_ocorrencia::text || new.payload::text,
        'sha256');
    return new;
end $$ language plpgsql;

create trigger trg_evento_hash before insert on evento
    for each row execute function fn_evento_hash();

-- Append-only na marra. Complementar com:
--   revoke update, delete on evento from app_role;
create or replace function fn_evento_imutavel() returns trigger as $$
begin
    raise exception 'evento é append-only. Use tipo=CORRECAO.';
end $$ language plpgsql;

create trigger trg_evento_no_update before update or delete on evento
    for each row execute function fn_evento_imutavel();

-- ------------------------------------------------------------
-- 4. TRANSFORMAÇÃO — N entradas, M saídas
--    Mesa densimétrica, eletrônica, blend, big bag.
--    O modelo pai/filho do protótipo não expressa isto.
-- ------------------------------------------------------------

create table transformacao (
    transformacao_id uuid primary key default gen_random_uuid(),
    evento_id     uuid not null references evento,
    tipo          text not null check (tipo in (
                      'DENSIMETRICA','ELETRONICA','PENEIRAMENTO',
                      'BLEND','ENSACAMENTO','REBENEFICIO')),
    ts_ocorrencia timestamptz not null
);

create table transformacao_entrada (
    transformacao_id uuid not null references transformacao,
    lote_id       uuid not null references lote,
    peso_kg       numeric(12,3) not null,
    primary key (transformacao_id, lote_id)
);

create table transformacao_saida (
    transformacao_id uuid not null references transformacao,
    lote_id       uuid not null references lote,
    peso_kg       numeric(12,3) not null,
    fracao        text,        -- 'BOM','MEIO_A_MEIO','RESIDUO'
    primary key (transformacao_id, lote_id)
);

-- Rendimento e perda ficam explícitos. Se não fecha, alguém erra.
create view vw_rendimento as
select t.transformacao_id, t.tipo,
       sum(distinct e.peso_kg) as entrada_kg,
       sum(distinct s.peso_kg) as saida_kg,
       round(sum(distinct s.peso_kg) / nullif(sum(distinct e.peso_kg),0) * 100, 2) as rendimento_pct
  from transformacao t
  join transformacao_entrada e using (transformacao_id)
  join transformacao_saida   s using (transformacao_id)
 group by t.transformacao_id, t.tipo;

-- ------------------------------------------------------------
-- 5. OCUPAÇÃO DE TULHA
--    Se uma tulha recebe 2 lotes, a rastreabilidade quebra
--    fisicamente. Registrar a composição é a única defesa.
-- ------------------------------------------------------------

create table tulha_ocupacao (
    ocupacao_id   uuid primary key default gen_random_uuid(),
    tulha_id      uuid not null references tulha,
    lote_id       uuid not null references lote,
    peso_kg       numeric(12,3) not null,
    entrada_em    timestamptz not null,
    saida_em      timestamptz,
    evento_entrada_id uuid not null references evento,
    evento_saida_id   uuid references evento
);

-- Uma tulha dedicada só aceita um lote por vez.
create unique index uq_tulha_dedicada
    on tulha_ocupacao (tulha_id)
 where saida_em is null;

-- ------------------------------------------------------------
-- 6. AMOSTRA E CLASSIFICAÇÃO
--    5 amostras de 1kg no terreirinho; 1 a cada 15 bags no armazém.
-- ------------------------------------------------------------

create table amostra (
    amostra_id    uuid primary key default gen_random_uuid(),
    lote_id       uuid not null references lote,
    evento_id     uuid not null references evento,
    etiqueta_cor  text check (etiqueta_cor in
                     ('BRANCO','AMARELO','AZUL','ROSA')),
    peso_g        numeric(8,2),
    pct_boia      numeric(5,2),
    pct_cereja    numeric(5,2),
    pct_verde     numeric(5,2),
    padrao_mk     text,                     -- 'M3+', 'M7'
    peneira       text,                     -- 'MK10','13+','14+'
    defeitos      int,
    cata_pct      numeric(5,2)
);

-- ------------------------------------------------------------
-- 7. GANCHOS PARA AS FASES SEGUINTES
--    Criar agora, popular depois. Migração retroativa custa caro.
-- ------------------------------------------------------------

-- Fase 3: etiqueta física (QR agora, RFID depois).
create table unidade_fisica (
    unidade_id    uuid primary key default gen_random_uuid(),
    codigo        text not null unique,     -- QR agora; EPC quando houver UHF
    tipo          text not null check (tipo in ('SACA','BIG_BAG','CAIXA','AMOSTRA')),
    tara_kg       numeric(8,3),
    epc           text unique,              -- nulo até a fase RFID
    criado_em     timestamptz not null default now()
);

-- Vínculo unidade <-> lote. N:1, versionado. Sem UPDATE.
create table unidade_vinculo (
    vinculo_id    uuid primary key default gen_random_uuid(),
    unidade_id    uuid not null references unidade_fisica,
    lote_id       uuid not null references lote,
    evento_id     uuid not null references evento,
    inicio        timestamptz not null,
    fim           timestamptz
);

create unique index uq_unidade_ativa
    on unidade_vinculo (unidade_id) where fim is null;

-- Fase 4: dispositivos IoT. Identidade vem do certificado, não do payload.
create table dispositivo (
    device_id     text primary key,          -- = CN do certificado mTLS
    tipo          text not null check (tipo in ('SENSOR_SECAGEM','PORTAL_UHF','HANDHELD')),
    cert_fingerprint text unique,
    talhao_id     uuid references talhao,
    ativo         boolean not null default true
);

-- ------------------------------------------------------------
-- 8. VERIFICAÇÃO DA CADEIA
--    Roda no fechamento de lote e em auditoria.
-- ------------------------------------------------------------

create or replace function fn_verifica_cadeia(p_lote uuid)
returns table (evento_id uuid, seq bigint, integro boolean) as $$
declare
    r record; v_prev bytea := null; v_calc bytea;
begin
    for r in select * from evento where lote_id = p_lote order by seq_lote loop
        v_calc := digest(
            coalesce(encode(v_prev,'hex'),'') ||
            r.lote_id::text || r.tipo || r.usuario_id::text ||
            r.ts_ocorrencia::text || r.payload::text,
            'sha256');
        evento_id := r.evento_id;
        seq       := r.seq_lote;
        integro   := (v_calc = r.hash);
        return next;
        v_prev := r.hash;
    end loop;
end $$ language plpgsql;

-- ------------------------------------------------------------
-- 9. VISÃO DE RASTREIO — "de onde veio este café?"
-- ------------------------------------------------------------

create or replace view vw_linha_tempo as
select e.lote_id, l.referencia, l.codigo_mtp,
       e.seq_lote, e.tipo, e.ts_ocorrencia, e.origem,
       u.nome as operador, e.payload
  from evento e
  join lote l using (lote_id)
  join usuario u on u.usuario_id = e.usuario_id
 order by e.lote_id, e.seq_lote;

-- Origem recursiva através de transformações.
create or replace view vw_origem_recursiva as
with recursive origem as (
    select s.lote_id as lote_final, e.lote_id as lote_origem, 1 as nivel
      from transformacao_saida s
      join transformacao_entrada e using (transformacao_id)
    union all
    select o.lote_final, e.lote_id, o.nivel + 1
      from origem o
      join transformacao_saida s on s.lote_id = o.lote_origem
      join transformacao_entrada e using (transformacao_id)
     where o.nivel < 10
)
select o.lote_final, o.lote_origem, o.nivel,
       t.codigo as talhao, t.centroide
  from origem o
  join lote l on l.lote_id = o.lote_origem
  join talhao t using (talhao_id);
